// HTTP 服务实现（对应 Python app/main.py、api/routes_*.py、api/deps.py、api/errors.py）
// 错误语义、字段、ETag 严格按 docs/PORT_SPEC.md §1.3 / §4.6。
#include "lnv/http/server.h"

#include "lnv/auth.h"
#include "lnv/http/validate.h"
#include "lnv/layout/engine.h"
#include "lnv/model.h"
#include "lnv/netinfo.h"
#include "lnv/render/raster.h"
#include "lnv/util/imaging.h"
#include "lnv/util/md5.h"
#include "lnv/util/sha1.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <windows.h>   // GetFileAttributesExW（字体文件 ETag 需要与 CPython 完全一致的 mtime）

namespace lnv {
namespace {

using json = nlohmann::json;
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

// ------------------------------------------------------------ 小工具

// 优雅退出标志与当前 server 指针（Ctrl 处理器 → request_http_stop）。
// g_server 必须是原子：listen 线程写、控制台 Ctrl 线程读，普通指针是数据竞争（UB，审查 L12）。
std::atomic<bool> g_stop_requested{false};
std::atomic<httplib::Server*> g_server{nullptr};  // 仅 listen 期间有效

std::optional<std::string> header_opt(const httplib::Request& req, const char* name) {
    if (req.has_header(name)) return req.get_header_value(name);
    return std::nullopt;
}

// If-None-Match 头 → 可选指针（etag_matches 需要 const std::string*）
const std::string* inm_ptr(const httplib::Request& req) {
    thread_local std::string buf;
    if (!req.has_header("If-None-Match")) return nullptr;
    buf = req.get_header_value("If-None-Match");
    return &buf;
}

// query 参数（httplib 的 params 已做 URL 解码）；nullopt 表示缺失。
// 取值与校验一律走 pv_*（pydantic 等价），不要在这里做类型转换或 clamp：
// Python 参照版对越界/非法参数返回 400 + detail.errors，静默 clamp 会让客户端
// 把"参数写错"当成"服务器照做了"（见 .e2e/review/diff_http.py 的差异清单）。
std::optional<std::string> qparam(const httplib::Request& req, const char* name) {
    if (!req.has_param(name)) return std::nullopt;
    return req.get_param_value(name);
}

// POST body → JSON；失败 → 400 + detail.errors[{loc:["body",pos],…}]
json parse_body(const httplib::Request& req) { return pv_parse_body(req.body); }

// Python str.lower() 的 ASCII 等价（format/fit 枚举大小写不敏感）
std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim_ascii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

// 读整个文件（二进制）；失败返回 false
bool read_file_bytes(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Range 单段解析（§4.6，逐行对齐 Python routes_read.py::_slice_range）：
// 语法无效（RFC 9110 要求忽略）→ None（按 200 整页）；语法有效但越界 → Unsatisfiable（416）。
enum class RangeKind { None, Ok, Unsatisfiable };
struct RangeSpec {
    long long start = 0, end = 0;
    std::string content_range;
};

RangeKind parse_range(const std::string& header, long long total, RangeSpec& out) {
    std::string low = lower_ascii(header);
    static const std::string prefix = "bytes=";
    if (low.size() < prefix.size() || low.compare(0, prefix.size(), prefix) != 0)
        return RangeKind::None;
    std::string spec = header.substr(prefix.size());
    auto comma = spec.find(',');  // 忽略多段，取第一段
    if (comma != std::string::npos) spec = spec.substr(0, comma);
    spec = trim_ascii(spec);
    auto dash = spec.find('-');
    if (dash == std::string::npos) return RangeKind::None;
    std::string a = trim_ascii(spec.substr(0, dash));
    std::string b = trim_ascii(spec.substr(dash + 1));
    // 十进制解析（饱和到 long long 边界）。Python 的 int 无界：`bytes=99999999999999999999-`
    // 必须判为"超出文件"→416，`bytes=-99999999999999999999` 夹到 0..total-1→206。
    // 用 std::stoll 会抛 out_of_range → 退化成"语法非法→200 整页"（审查 M5）。
    auto to_ll = [](const std::string& s, long long& v) -> bool {
        if (s.empty()) {
            v = 0;
            return true;
        }
        size_t i = 0;
        bool neg = false;
        if (s[i] == '+') i++;
        else if (s[i] == '-') { neg = true; i++; }
        if (i >= s.size()) return false;
        unsigned long long acc = 0;
        bool sat = false;
        for (; i < s.size(); i++) {
            if (!std::isdigit((unsigned char)s[i])) return false;
            unsigned d = unsigned(s[i] - '0');
            if (!sat) {
                if (acc > (0x7FFFFFFFFFFFFFFFull - d) / 10) sat = true;
                else acc = acc * 10 + d;
            }
        }
        v = sat ? (neg ? LLONG_MIN : LLONG_MAX) : (neg ? -(long long)acc : (long long)acc);
        return true;
    };
    long long start = 0, end = 0;
    if (a.empty()) {
        if (b.empty()) return RangeKind::None;  // "bytes=-"
        long long length = 0;
        if (!to_ll(b, length)) return RangeKind::None;
        start = std::max(0LL, total - length);
        end = total - 1;
    } else {
        if (!to_ll(a, start)) return RangeKind::None;
        if (b.empty()) {
            end = total - 1;
        } else if (!to_ll(b, end)) {
            return RangeKind::None;
        }
    }
    if (start < 0 || end < 0) return RangeKind::None;
    if (start >= total) {
        out.content_range = "bytes */" + std::to_string(total);
        return RangeKind::Unsatisfiable;
    }
    end = std::min(end, total - 1);
    if (end < start) {
        out.content_range = "bytes */" + std::to_string(total);
        return RangeKind::Unsatisfiable;
    }
    out.start = start;
    out.end = end;
    out.content_range = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                        std::to_string(total);
    return RangeKind::Ok;
}

// ---------------------------------------------------------------- 错误映射
// 统一异常 → HTTP 响应（§1.2）。401 时附加 WWW-Authenticate: X-Auth-Token。
void send_error(const httplib::Request&, httplib::Response& res, const std::exception& e) {
    std::fprintf(stderr, "[error] %s\n", e.what());
    // handler 可能已经设过 ETag / Cache-Control / X-Page-* 等成功响应头（例如
    // 页面渲染中途失败）。Python 的异常处理器返回的是全新 JSONResponse，不带这些头；
    // 留着它们会让客户端把 4xx/5xx 当可缓存内容（Cache-Control: immutable）。
    res.headers.clear();
    ErrorCode code = ErrorCode::Internal;
    // 消息里可能混入非 UTF-8 字节（路径经 ANSI 转换），JSON dump 会抛 → 先消毒
    std::string what;
    {
        const unsigned char* p = (const unsigned char*)e.what();
        size_t n = strlen(e.what());
        what.reserve(n);
        for (size_t i = 0; i < n;) {
            unsigned char c = p[i];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                        : (c >> 3) == 0x1E ? 4 : 0;
            bool valid = len > 0 && i + len <= n;
            if (valid)
                for (size_t k = 1; k < len; k++)
                    if ((p[i + k] >> 6) != 2) { valid = false; break; }
            if (valid) {
                what.append(e.what() + i, len);
                i += len;
            } else {
                what.push_back('?');
                i++;
            }
        }
    }

    json body;
    const json* detail = nullptr;
    int status_override = 0;
    if (const auto* ae = dynamic_cast<const AppError*>(&e)) {
        code = ae->code;
        detail = &ae->detail;
        status_override = ae->http_status;
    } else if (dynamic_cast<const ServerBusyError*>(&e)) {
        code = ErrorCode::ServerBusy;
        static const json busy = json::object({{"reason", "busy"}});
        detail = &busy;
    } else if (dynamic_cast<const ParseTimeoutError*>(&e)) {
        code = ErrorCode::ParseFailed;
        static const json timeout = json::object({{"reason", "timeout"}});
        detail = &timeout;
    } else if (const auto* uf = dynamic_cast<const UnsupportedFormatError*>(&e)) {
        code = ErrorCode::UnsupportedFormat;
        // Python：detail = {"reason": …} if reason else {}（空 reason 时整个 detail 键消失）
        json d = uf->reason.empty() ? json(nullptr) : json::object({{"reason", uf->reason}});
        body = error_body(code, what.c_str(), d);
        res.status = error_http_status(code);
        res.set_content(body.dump(), "application/json");
        return;
    } else if (const auto* itl = dynamic_cast<const ImageTooLargeError*>(&e)) {
        // errors.py: 422 PARSE_FAILED + detail {"reason":"image_too_large"}
        body = error_body(ErrorCode::ParseFailed, what.c_str(),
                          json::object({{"reason", "image_too_large"}}));
        res.status = 422;
        res.set_content(body.dump(), "application/json");
        return;
    } else if (const auto* ide = dynamic_cast<const ImageDecodeError*>(&e)) {
        // errors.py: 422 PARSE_FAILED + detail {"reason":"image_decode"}
        body = error_body(ErrorCode::ParseFailed, what.c_str(),
                          json::object({{"reason", "image_decode"}}));
        res.status = 422;
        res.set_content(body.dump(), "application/json");
        return;
    } else if (dynamic_cast<const ParseError*>(&e)) {
        code = ErrorCode::ParseFailed;
    } else if (dynamic_cast<const AssetNotFoundError*>(&e)) {
        code = ErrorCode::AssetNotFound;
    } else if (dynamic_cast<const PathError*>(&e)) {
        code = ErrorCode::BadPath;
    } else if (dynamic_cast<const TraversalError*>(&e)) {
        code = ErrorCode::PathTraversal;
    } else {
        // 其它 → 500 INTERNAL detail {"type":"unknown"}
        static const json unknown = json::object({{"type", "unknown"}});
        body = error_body(ErrorCode::Internal, "服务器内部错误", unknown);
        res.status = 500;
        res.set_content(body.dump(), "application/json");
        return;
    }
    body = error_body(code, what.c_str(), detail && !detail->is_null() ? *detail : json(nullptr));
    res.status = status_override ? status_override : error_http_status(code);
    if (code == ErrorCode::Unauthorized)
        res.set_header("WWW-Authenticate", "X-Auth-Token");
    if (code == ErrorCode::ServerBusy)
        res.set_header("Retry-After", "5");
    res.set_content(body.dump(), "application/json");
}

// BOOK_NOT_FOUND 的统一构造
AppError book_not_found(const std::string& book_id) {
    return AppError(ErrorCode::BookNotFound, "书不存在或已被移除",
                    json::object({{"book_id", book_id}}));
}

// layout_id 必填（已由 Validator 校验）+ 条目查找（LAYOUT_NOT_FOUND）
LayoutEntry require_entry(AppContext& ctx, const std::string& book_id,
                          const std::string& layout_id) {
    auto entry = ctx.require_layout(book_id, layout_id);
    if (!entry)
        throw AppError(ErrorCode::LayoutNotFound, "layout_id 不存在或已失效，请重新 POST /layout",
                       json::object({{"book_id", book_id}, {"layout_id", layout_id}}));
    return *entry;
}

// handler 统一包装：鉴权（可豁免）+ 异常映射 + 访问日志（METHOD path -> status 耗时ms）
// head_405=false 用于兜底路由：Python/Starlette 先做路由匹配，未知路径的 HEAD 是 404
// 而不是 405（审查 M6）。
Handler wrap(AppContext& ctx, bool require_auth, Handler fn, bool head_405 = true) {
    return [&ctx, require_auth, head_405, fn = std::move(fn)](const httplib::Request& req,
                                                    httplib::Response& res) {
        const auto t0 = std::chrono::steady_clock::now();
        // httplib 在路由前已按 RFC 解析了 Range 头，并在写响应时**再**对响应体做一次
        // 分块（range_error/apply_ranges）。Range 语义由本文件的 §4.6 实现负责，
        // 这里清空解析结果，避免二次分块（否则出现两段 Content-Range、多段请求被
        // 误判 416、206 响应体被再次截断）。req 在 httplib 内部是非 const 对象。
        const_cast<httplib::Request&>(req).ranges.clear();
        try {
            // Python/FastAPI 的 @get 路由不接受 HEAD（实测 HEAD /healthz → 405
            // Method Not Allowed）。httplib 会把 HEAD 派发给 GET 处理器，因此这里
            // 显式拒绝，保证两边状态一致。
            if (head_405 && req.method == "HEAD")
                throw AppError(ErrorCode::BadRequest, "Method Not Allowed", nullptr, 405);
            if (require_auth)
                require_token(ctx.cfg, header_opt(req, "X-Auth-Token"));
            fn(req, res);
        } catch (const std::exception& e) {
            try { send_error(req, res, e); }
            catch (...) { res.status = 500; }
        } catch (...) {
            // 非 std::exception（如第三方库抛出的整型/枚举）：500 INTERNAL
            try {
                res.headers.clear();
                res.status = 500;
                res.set_content(lnv::error_body(ErrorCode::Internal, "服务器内部错误",
                                                json::object({{"type", "unknown"}})).dump(),
                                "application/json");
            } catch (...) { res.status = 500; }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        // 未显式设置状态时 httplib 会补 200（有 Range 时 206）——日志按同一规则显示，
        // 否则全是 "-1"，排查问题时分不清成功与失败。
        const int logged = res.status != -1 ? res.status : (req.ranges.empty() ? 200 : 206);
        std::printf("%s %s -> %d %lldms\n", req.method.c_str(), req.path.c_str(),
                    logged, static_cast<long long>(ms));
        std::fflush(stdout);
    };
}

// ---------------------------------------------------------------- 端点实现

// GET /api/v1/server/info（不鉴权；routes_server.py）
void handle_server_info(AppContext& ctx, const httplib::Request&, httplib::Response& res) {
    const auto& cfg = ctx.cfg;
    json libs = json::array();
    for (const auto& l : ctx.library->libraries_info())
        libs.push_back({{"name", l.name}, {"book_count", l.book_count}, {"path", nullptr}});
    json j = {
        {"name", cfg.server.name},
        {"version", "0.1.0"},
        {"api_version", "v1"},
        {"auth_required", cfg.auth_required()},
        {"libraries", libs},
        {"features", {
            {"png", true}, {"json", true}, {"search", true},
            {"gallery", cfg.gallery.folder_as_book},
            {"fits", json::array({"viewport", "original"})},
            {"strip", true}, {"progress_sync", true},
            {"strip_min_aspect", cfg.comic.strip_min_aspect},
        }},
        {"server_time", iso_time_utc(now_epoch())},
    };
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/files（routes_files.py）：library.list_dir 直接输出；path=="/" 补 libraries
void handle_files(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    Validator v;
    const std::string path = pv_str(v, qparam(req, "path"), {"query", "path"}, "/", false, 0);
    const long long limit =
        pv_int(v, qparam(req, "limit"), {"query", "limit"}, 1000, int_range(1, 5000));
    const long long offset = pv_int(v, qparam(req, "offset"), {"query", "offset"}, 0, int_ge(0));
    v.done();

    json j = ctx.library->list_dir(path, static_cast<int>(limit), offset);
    if (path == "/") {
        json libs = json::array();
        for (const auto& l : ctx.library->libraries_info())
            libs.push_back({{"name", l.name}, {"book_count", l.book_count},
                            {"path", l.path ? json(*l.path) : json(nullptr)}});
        j["libraries"] = libs;
    }
    // 副作用：索引节流落盘（最短间隔 30s）
    ctx.library->maybe_save();
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/books/{id}/meta（routes_books.py）
void handle_book_meta(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    auto adapter = ctx.open_adapter(*ref);
    auto meta = adapter->meta();
    // ChapterInfo 契约：index/title/total_chars?/start_page? 四个键恒在（routes_books.py
    // 用 ChapterInfo(...) 构造，缺省字段序列化为 null）。适配器只填自己知道的字段，
    // 这里统一补齐，否则客户端按键取值会拿到 undefined。
    json chapters = json::array();
    for (size_t i = 0; i < meta.chapters.size(); i++) {
        const json& c = meta.chapters[i];
        std::string title = c.value("title", std::string());
        if (title.empty()) title = "第 " + std::to_string(i + 1) + " 节";
        chapters.push_back({{"index", c.value("index", static_cast<int>(i))},
                            {"title", title},
                            {"total_chars", c.contains("total_chars") ? c["total_chars"]
                                                                     : json(nullptr)},
                            {"start_page", c.contains("start_page") ? c["start_page"]
                                                                    : json(nullptr)}});
    }
    // 内容修订号（跨实现契约）：文件书 = sha1(book_id:size:mtime_int)[:12]；
    // 图集虚拟书在 require_book 时已算好。cover_url 必须带 rev。
    const std::string rev = ref->rev;
    json j = {
        {"book_id", book_id},
        {"title", meta.title.empty() ? ref->title_guess() : meta.title},
        {"author", meta.author ? json(*meta.author) : json(nullptr)},
        {"format", meta.format.empty() ? ref->format : meta.format},
        {"language", meta.language ? json(*meta.language) : json(nullptr)},
        {"size", ref->size},
        {"mode", meta.mode},
        {"page_count", meta.page_count ? json(*meta.page_count) : json(nullptr)},
        {"revision", rev},
        {"cover_url", "/api/v1/books/" + book_id + "/cover?rev=" + rev},
        {"render_support", {{"png", true}, {"json", adapter->supports_json()}}},
        {"chapters", chapters},
        {"source", ref->vpath},
    };
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/books/{id}/cover（routes_books.py）
void handle_cover(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    Validator v;
    const long long w =
        pv_int(v, qparam(req, "w"), {"query", "w"}, 240, int_range(40, 1024));
    v.done();
    // 契约：先解析书（被删除 → 404），用**当前** rev 计算 ETag；绝不在未解析书的
    // 情况下命中 304（书被删/内容变化后旧 ETag 必须失效）。
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    const std::string rev = ref->rev;
    const std::string etag = "\"" + book_id + ":" + rev + ":w" + std::to_string(w) + "\"";
    res.set_header("ETag", etag);
    res.set_header("Cache-Control", "public, max-age=86400");
    if (ctx.etag_matches(inm_ptr(req), etag)) { res.status = 304; return; }
    // 内存缓存键 ("cover", book_id, rev, w)；缓存值 = mime + '\n' + 字节
    const std::string key = "cover:" + book_id + ":" + rev + ":" + std::to_string(w);
    std::string bytes;
    std::string mime = "image/jpeg";
    if (auto hit = ctx.cover_cache->get(key)) {
        auto nl = hit->find('\n');
        mime = hit->substr(0, nl);
        bytes = hit->substr(nl + 1);
    } else {
        auto adapter = ctx.open_adapter(*ref);
        ImageBytes img;
        if (auto cov = adapter->cover(static_cast<int>(w))) {
            img = std::move(*cov);
        } else {
            // 无内嵌封面 → 文字封面（JPEG q88）
            auto meta = adapter->meta();
            img = text_cover(meta.title.empty() ? ref->title_guess() : meta.title,
                             meta.author, static_cast<int>(w), ctx.fonts.get());
            img.mime = "image/jpeg";
        }
        bytes = std::move(img.bytes);
        mime = img.mime.empty() ? "image/jpeg" : img.mime;
        ctx.cover_cache->put(key, mime + "\n" + bytes,
                             static_cast<long long>(bytes.size()));
    }
    res.set_content(bytes, mime);
}

// GET /api/v1/books/{id}/asset（routes_books.py）
void handle_asset(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    Validator v;
    const std::string raw_path =
        pv_str(v, qparam(req, "path"), {"query", "path"}, "", /*required=*/true, 0);
    v.done();
    const std::string safe = safe_asset_path(raw_path);
    const std::string etag = "\"" + book_id + ":asset:" + stable_hash(safe, 16) + "\"";
    res.set_header("ETag", etag);
    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
    if (ctx.etag_matches(inm_ptr(req), etag)) { res.status = 304; return; }
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    auto adapter = ctx.open_adapter(*ref);
    ImageBytes img = adapter->asset(safe);
    if (img.mime.empty()) img.mime = sniff_mime(img.bytes);
    res.set_content(img.bytes, img.mime);
}

// POST /api/v1/books/open-folder（routes_books.py）
void handle_open_folder(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    json body = parse_body(req);
    Validator v;
    pv_require_object(v, body, {"body"});
    const std::string path = jv_str(v, body, "path", {"body", "path"}, "", /*required=*/true);
    // recursive 走 pydantic 的宽松布尔语义：非布尔 → 400 bool_parsing；"true"/1 → true；
    // 缺失或 null → None（Python 是 Optional[bool]）。此前只在 is_boolean() 时赋值，
    // 非法值被静默忽略（审查 M8）。
    std::optional<bool> recursive;
    if (body.contains("recursive") && !body["recursive"].is_null())
        recursive = jv_bool(v, body, "recursive", {"body", "recursive"}, false);
    v.done();
    if (!ctx.cfg.gallery.folder_as_book)
        // Python routes_books.py:141-142 用 AppError(UNSUPPORTED_FORMAT, msg)：无 detail
        throw AppError(ErrorCode::UnsupportedFormat, "服务器已关闭“文件夹作为图集”功能");
    auto r = ctx.gallery->register_folder(path, recursive);
    // Python：cover_url 为空时回落到 /api/v1/books/{id}/cover（恒为字符串，不是 null）
    const std::string cover = r.cover_url.empty()
                                  ? "/api/v1/books/" + r.book_id + "/cover"
                                  : r.cover_url;
    json j = {
        {"book_id", r.book_id}, {"title", r.title}, {"format", "gallery"},
        {"page_count", r.page_count},
        {"cover_url", cover},
        {"mode", "fixed"},
    };
    res.set_content(j.dump(), "application/json");
}

// POST /api/v1/books/{id}/layout（routes_read.py）
void handle_layout(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    // FastAPI 顺序：请求体校验（依赖求解）→ handler 内 require_book
    json body = parse_body(req);
    LayoutProfile profile = LayoutProfile::from_json(body);
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    auto adapter = ctx.open_adapter(*ref);
    auto r = ctx.layouts->build(*ref, *adapter, profile);
    json chapters = json::array();
    for (const auto& c : r.chapters) chapters.push_back(c);
    // revision：当前内容的修订号（页面/封面 URL 与 ETag 都以它做失效维度）
    const std::string rev = ref->rev;
    json j = {
        {"layout_id", r.layout_id}, {"mode", r.mode}, {"total_pages", r.total_pages},
        {"chapters", chapters}, {"cache_hit", r.cache_hit}, {"strip", r.strip},
        {"revision", rev},
    };
    res.set_content(j.dump(), "application/json");
}

// 页码解析与越界校验（§1.3 校验顺序第 4 步）。
// 参数用 long long：Python 的路径参数是无界 int，`pages/4294967297` 必须报
// PAGE_OUT_OF_RANGE 并**回显原始值**，不能先窄化成 int（否则 4294967297→1 会当成
// 合法页返回图片；审查 H4）。
int check_page(const PageMap& pm, long long page) {
    if (page < 1 || page > (long long)pm.total_pages)
        throw AppError(ErrorCode::PageOutOfRange,
                       "页码越界：" + std::to_string(page) + " / " +
                           std::to_string(pm.total_pages),
                       json::object({{"page", page}, {"total_pages", pm.total_pages}}));
    return static_cast<int>(page);
}

// GET /api/v1/books/{id}/pages/{page}（routes_read.py，校验顺序 §1.3）
void handle_page(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    // 1) 参数校验（FastAPI 在进入 handler 前完成，顺序在 format/fit 枚举之前）
    Validator v;
    const std::string page_raw = req.matches[2].str();
    const long long page_ll =
        pv_int(v, std::optional<std::string>(page_raw), {"path", "page"}, 0, IntBound{});
    const std::string layout_id =
        pv_str(v, qparam(req, "layout_id"), {"query", "layout_id"}, "", /*required=*/true, 0);
    const std::string theme = pv_str(v, qparam(req, "theme"), {"query", "theme"}, "white", false, 0);
    const double scale =
        pv_num(v, qparam(req, "scale"), {"query", "scale"}, 1.0, num_range(0.5, 2.0));
    std::optional<int> quality;
    if (qparam(req, "quality"))
        quality = static_cast<int>(
            pv_int(v, qparam(req, "quality"), {"query", "quality"}, 85, int_range(1, 100)));
    const bool pair =
        pv_int(v, qparam(req, "pair"), {"query", "pair"}, 0, int_range(0, 1)) == 1;
    const bool rtl = pv_int(v, qparam(req, "rtl"), {"query", "rtl"}, 0, int_range(0, 1)) == 1;
    v.done();

    // 2) format / fit 枚举（handler 内校验：detail 带客户端原始值，非小写化后的值）
    std::string format_raw =
        pv_str(v, qparam(req, "format"), {"query", "format"}, "png", false, 0);
    if (format_raw.empty()) format_raw = "png";
    std::string fit_raw = pv_str(v, qparam(req, "fit"), {"query", "fit"}, "viewport", false, 0);
    if (fit_raw.empty()) fit_raw = "viewport";
    const std::string format = lower_ascii(format_raw);
    const std::string fit = lower_ascii(fit_raw);
    if (format != "png" && format != "json")
        throw AppError(ErrorCode::BadRequest, "format 只支持 png/json",
                       json::object({{"format", format_raw}}));
    if (fit != "viewport" && fit != "original")
        throw AppError(ErrorCode::BadRequest, "fit 只支持 viewport/original",
                       json::object({{"fit", fit_raw}}));

    // 3) book → 4) layout → 5) 页码
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    auto entry = require_entry(ctx, book_id, layout_id);
    const PageMap& pm = entry.pagemap;
    const int page = check_page(pm, page_ll);

    const bool fixed = pm.mode != "flow";  // fixed / strip

    // §4.6：ETag / 缓存键 / 公共响应头（png 与 json 均带）。
    // rev 为第二段：内容变化后 ETag 与内存缓存键都随 rev 变化。
    const std::string rev = ref->rev;
    const std::string etag =
        ctx.page_etag(layout_id, rev, page, theme, scale, fit, quality, pair, rtl, fixed);
    const std::string cache_key =
        ctx.page_cache_key(book_id, layout_id, rev, page, theme, scale, fit, quality, pair,
                           rtl, fixed);
    res.set_header("ETag", etag);
    res.set_header("Cache-Control", "private, max-age=31536000, immutable");
    res.set_header("X-Page-Index", std::to_string(page));
    res.set_header("X-Total-Pages", std::to_string(pm.total_pages));
    res.set_header("X-Layout-Id", layout_id);
    if (ctx.etag_matches(inm_ptr(req), etag)) { res.status = 304; return; }

    // ---------------- flow ----------------
    if (!fixed) {
        auto adapter = ctx.open_adapter(*ref);
        if (format == "json") {
            // JSON 模式仅 flow；不占页面图片缓存
            auto doc = ctx.layouts->document(*ref, *adapter);
            json j = build_page_json(doc, pm, page, entry.profile, ctx.fonts.get(), book_id);
            res.set_content(j.dump(), "application/json");
            return;
        }
        std::string mime = "image/png";
        std::string bytes;
        if (auto hit = ctx.page_cache->get(cache_key)) {
            auto nl = hit->find('\n');
            mime = hit->substr(0, nl);
            bytes = hit->substr(nl + 1);
        } else {
            auto doc = ctx.layouts->document(*ref, *adapter);
            RenderDeps deps;
            deps.fonts = ctx.fonts.get();
            deps.base_dir = &ctx.cfg.base_dir;
            deps.asset_loader = [&adapter](const std::string& p, std::string& out) -> bool {
                try {
                    ImageBytes img = adapter->asset(p);
                    out = std::move(img.bytes);
                    return true;
                } catch (...) { return false; }
            };
            deps.max_source_pixels = ctx.cfg.limits.max_source_pixels;
            deps.max_page_pixels = ctx.cfg.render.max_page_pixels;
            deps.jpeg_for_photo_pages = ctx.cfg.render.jpeg_for_photo_pages;
            deps.theme = theme;
            deps.scale = scale;
            ImageBytes img = render_flow_page(doc, pm, page, entry.profile, deps);
            bytes = std::move(img.bytes);
            mime = img.mime.empty() ? "image/png" : img.mime;
            ctx.page_cache->put(cache_key, mime + "\n" + bytes,
                                static_cast<long long>(bytes.size()));
        }
        res.set_content(bytes, mime);
        return;
    }

    // ---------------- fixed / strip ----------------
    if (format == "json")
        throw AppError(ErrorCode::BadRequest,
                       "固定版式（PDF/漫画/图集）不支持 format=json，请使用 format=png",
                       json::object({{"mode", pm.mode}}));

    auto adapter = ctx.open_adapter(*ref);
    // 渲染用配置质量；etag/key 中 quality 缺省为 nullopt
    const int render_q = quality.value_or(ctx.cfg.comic.jpeg_quality);
    // Python render/raster.py::fixed_page_bytes：scale 先折算进 viewport_w
    // （viewport.w × clamp_scale(scale, profile, max_page_pixels)），适配器内部再乘
    // render_scale，因此这里必须传 1.0；用 avail_width 或不 clamp 会让请求的倍率与
    // max_page_pixels 双双失效（2000×3000 的视口 + scale=2 会渲染出 4000px 宽的页）。
    const int viewport_w =
        std::max(1, static_cast<int>(std::lround(
                        entry.profile.viewport.w *
                        clamp_scale(scale, entry.profile, ctx.cfg.render.max_page_pixels))));
    const double render_scale = 1.0;

    // Range 仅 fixed/strip 且 fit=original（§4.6）
    const bool allow_range = fit == "original";
    if (allow_range) res.set_header("Accept-Ranges", "bytes");

    auto fetch_page = [&]() -> ImageBytes {
        if (auto hit = ctx.page_cache->get(cache_key)) {
            auto nl = hit->find('\n');
            return {hit->substr(nl + 1), hit->substr(0, nl)};
        }
        ImageBytes img;
        if (pm.mode == "strip") {
            // StripPage 从 pagemap record 构造：image_index / y0 / height
            const PageRecord* rec = pm.record(page);
            StripPage sp;
            sp.image_index = rec->image_index;
            sp.y0 = static_cast<long long>(rec->y0);
            sp.height = static_cast<long long>(rec->height);
            img = adapter->strip_slice_bytes(sp, render_q, viewport_w, render_scale, fit);
        } else if (pair) {
            img = adapter->pair_bytes(page, render_q, viewport_w, render_scale, rtl);
        } else {
            img = adapter->page_bytes(page, fit, render_q, viewport_w, render_scale);
        }
        if (img.mime.empty()) img.mime = sniff_mime(img.bytes);
        // 与 Python routes_read.py:278-298 一致：**整页字节无条件入内存缓存**
        // （cache_key 已含 fit）。此前 fit=original 不入缓存，导致每次原图/Range
        // 请求都重新解码+编码（审查 M11）。
        ctx.page_cache->put(cache_key, img.mime + "\n" + img.bytes,
                            static_cast<long long>(img.bytes.size()));
        return img;
    };

    ImageBytes img = fetch_page();
    const long long total = static_cast<long long>(img.bytes.size());
    if (allow_range && req.has_header("Range")) {
        RangeSpec r;
        const RangeKind kind = parse_range(req.get_header_value("Range"), total, r);
        if (kind == RangeKind::Unsatisfiable) {   // 语法有效但越界 → 416
            res.status = 416;
            res.set_header("Content-Range", r.content_range);
            return;
        }
        if (kind == RangeKind::Ok) {
            res.status = 206;
            res.set_header("Content-Range", r.content_range);
            res.set_content(img.bytes.substr(static_cast<size_t>(r.start),
                                             static_cast<size_t>(r.end - r.start + 1)),
                            img.mime);
            return;
        }
        // kind == None：语法无效的 Range 按 RFC 忽略，返回整页 200
    }
    res.set_content(img.bytes, img.mime);
}

// GET /api/v1/books/{id}/pages/{page}/meta（routes_read.py）
void handle_page_meta(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    Validator v;
    const std::string page_raw = req.matches[2].str();
    const long long page_ll =
        pv_int(v, std::optional<std::string>(page_raw), {"path", "page"}, 0, IntBound{});
    const std::string layout_id =
        pv_str(v, qparam(req, "layout_id"), {"query", "layout_id"}, "", /*required=*/true, 0);
    v.done();

    auto entry = require_entry(ctx, book_id, layout_id);
    const PageMap& pm = entry.pagemap;
    const int page = check_page(pm, page_ll);
    const PageRecord* rec = pm.record(page);

    // aspect 仅 fixed/strip；strip 用 image_index + 1（页号 ≠ 源图序号）；异常 → null
    // 顺序同 Python：先 layout/页码，只有需要 aspect 时才 require_book
    json aspect = nullptr;
    if (pm.mode != "flow") {
        try {
            auto ref = ctx.require_book(book_id);
            if (!ref) throw book_not_found(book_id);
            auto adapter = ctx.open_adapter(*ref);
            const int target_page = rec->image_index >= 0 ? rec->image_index + 1 : page;
            aspect = adapter->page_aspect(target_page);
        } catch (const std::exception&) {
            aspect = nullptr;
        }
    }
    json j = {
        {"page", page},
        {"total_pages", pm.total_pages},
        {"anchor", {{"chapter", rec->anchor[0]}, {"block", rec->anchor[1]},
                    {"char", rec->anchor[2]}}},
        {"chapter_index", rec->chapter_index},
        {"chapter_title", u32_to_utf8(rec->chapter_title)},
        {"first_line", u32_to_utf8(rec->first_line)},
        {"mode", pm.mode},
        {"aspect", aspect},
    };
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/books/{id}/locate（routes_read.py + layout/anchors.py §4.5）
// 注意：Python 的 locate **不**校验书是否存在，只查 layout（require_layout_async），
// 因此删掉的书若仍有 layout 缓存也能定位。保持一致以免同一请求两边给不同错误码。
void handle_locate(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    Validator v;
    const std::string layout_id =
        pv_str(v, qparam(req, "layout_id"), {"query", "layout_id"}, "", /*required=*/true, 0);
    const long long chapter =
        pv_int(v, qparam(req, "chapter"), {"query", "chapter"}, 0, int_ge(0));
    const long long block = pv_int(v, qparam(req, "block"), {"query", "block"}, 0, int_ge(0));
    const long long ch = pv_int(v, qparam(req, "char"), {"query", "char"}, 0, int_ge(0));
    v.done();

    auto entry = require_entry(ctx, book_id, layout_id);
    // 锚点用 long long：巨大值在 Python 里是"永远在文档之后"→ 定位到末页且 clamped=true。
    // 夹到 INT_MAX 后语义完全一致（真实锚点远小于 INT_MAX），而直接窄化会变成 0/负数
    // 从而定位到首页（审查 H4）。
    const auto clamp_int = [](long long x) {
        return static_cast<int>(std::min<long long>(x, 2147483647LL));
    };
    int target[3] = {clamp_int(chapter), clamp_int(block), clamp_int(ch)};
    LocateResult lr = locate(entry.pagemap, target);
    json j = {
        {"page", lr.page},
        {"total_pages", lr.total_pages},
        {"ratio", std::clamp((lr.page - 0.5) / std::max(1, lr.total_pages), 0.0, 1.0)},
        {"chapter_index", lr.chapter_index},
        {"chapter_title", u32_to_utf8(lr.chapter_title)},
        {"anchor_chapter_index",
         lr.anchor_chapter_index ? json(*lr.anchor_chapter_index) : json(nullptr)},
        {"clamped", lr.clamped},
    };
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/books/{id}/search（routes_read.py + services/search.py §4.10）
void handle_search(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    Validator v;
    const std::string q =
        pv_str(v, qparam(req, "q"), {"query", "q"}, "", /*required=*/true, /*min_len=*/1);
    const long long limit =
        pv_int(v, qparam(req, "limit"), {"query", "limit"}, 50, int_range(1, 500));
    v.done();

    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    // layout 存在但 mode≠flow → 415（entry 提到块外：pagemap 指针在块外仍需有效）
    const PageMap* pagemap = nullptr;
    std::optional<LayoutEntry> entry;
    // 空 layout_id 视同未提供（Python：`if layout_id:` 对空串为假）
    if (auto lid = qparam(req, "layout_id"); lid && !lid->empty()) {
        // Python 的 search 走 require_layout_async，但它的 detail **只有 layout_id**
        // （不含 book_id，与 /pages、/locate 不同）。require_layout 会抛带 book_id 的
        // 错误，所以这里先查缓存、查不到再按 search 的 detail 抛（审查 M7）。
        // 读穿 LayoutsService 的统一缓存（内存 + 磁盘），与 /layout 共用一套。
        entry = ctx.layouts->entry(book_id, *lid);
        if (!entry)
            throw AppError(ErrorCode::LayoutNotFound,
                           "layout_id 不存在或已失效，请重新 POST /layout",
                           json::object({{"layout_id", *lid}}));
        if (entry->pagemap.mode != "flow")
            // Python 这里抛的是 AppError(UNSUPPORTED_FORMAT, msg)（**无** detail）；
            // 用 UnsupportedFormatError 会多出 detail.reason（它默认取 message）
            throw AppError(ErrorCode::UnsupportedFormat, "该格式暂不支持全文搜索");
        pagemap = &entry->pagemap;
    }
    auto adapter = ctx.open_adapter(*ref);
    // adapter 非 flow（按格式）→ 415 detail {"format"}
    if (adapter->mode != "flow")
        throw AppError(ErrorCode::UnsupportedFormat,
                       adapter->format + " 暂不支持全文搜索",
                       json::object({{"format", adapter->format}}));
    auto doc = ctx.layouts->document(*ref, *adapter);
    json j = SearchService::search(doc, q, static_cast<int>(limit), pagemap);
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/progress/{book_id}（routes_progress.py §4.10）
void handle_progress_get(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    std::optional<ProgressService::Record> rec;
    // 空 device_id 视同未提供（Python：`None if device_id else get_any(...)`，
    // 空串是假值 → 回落到"最新一条"）
    if (auto dev = qparam(req, "device_id"); dev && !dev->empty())
        rec = ctx.progress->get(book_id, ProgressService::normalize_device(*dev));
    else
        rec = ctx.progress->get_any(book_id);  // 未指定 device → 最新一条
    if (!rec)
        throw AppError(ErrorCode::NotFound, "该书暂无进度记录",
                       json::object({{"book_id", book_id}}));
    res.set_content(rec->to_public_json().dump(), "application/json");
}

// PUT /api/v1/progress/{book_id}
void handle_progress_put(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string book_id = req.matches[1];
    // FastAPI 先做请求体校验（依赖求解阶段），再进入 handler 调 require_book
    json body = parse_body(req);
    Validator v;
    if (!body.is_object()) {
        v.add({"body"}, "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
        v.done();
    }
    // ProgressPutRequest.anchor 必填（Anchor 的 chapter/block/char 各有默认值 0）
    const json* anchor_obj = nullptr;
    auto it = body.find("anchor");
    if (it == body.end()) {
        v.add({"body", "anchor"}, "Field required", "missing");
    } else if (!it->is_object()) {
        v.add({"body", "anchor"},
              "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
    } else {
        anchor_obj = &*it;
    }
    nlohmann::json anchor[3] = {0, 0, 0};
    if (anchor_obj) {
        jv_int_ex(v, *anchor_obj, "chapter", {"body", "anchor", "chapter"}, 0, IntBound{},
                  false, false, &anchor[0]);
        jv_int_ex(v, *anchor_obj, "block", {"body", "anchor", "block"}, 0, IntBound{},
                  false, false, &anchor[1]);
        jv_int_ex(v, *anchor_obj, "char", {"body", "anchor", "char"}, 0, IntBound{},
                  false, false, &anchor[2]);
    }
    // page / ratio 可空（Optional[...]：缺失或 null 都取 None）；page 用 jv_int_ex
    // 保留原始 JSON 整数（Python 无界 int：2^63..2^64-1 原样存回并回显，见审查 H4）
    std::optional<nlohmann::json> page;
    if (body.contains("page") && !body["page"].is_null()) {
        nlohmann::json pv;
        jv_int_ex(v, body, "page", {"body", "page"}, 0, IntBound{}, false, true, &pv);
        if (pv.is_number()) page = std::move(pv);
    }
    std::optional<double> ratio;
    if (body.contains("ratio") && !body["ratio"].is_null())
        // 写入前 clamp [0,1]（ProgressPutRequest._clamp）
        ratio = std::clamp(jv_num(v, body, "ratio", {"body", "ratio"}, 0.0, NumBound{}, false, true),
                           0.0, 1.0);
    v.done();

    auto ref = ctx.require_book(book_id);
    if (!ref) throw book_not_found(book_id);
    std::optional<std::string> device;
    if (auto dev = qparam(req, "device_id")) device = *dev;
    auto rec = ctx.progress->put(book_id, device, anchor, page, ratio);
    res.set_content(rec.to_public_json().dump(), "application/json");
}

// GET /api/v1/render/fonts（routes_server.py）
void handle_fonts(AppContext& ctx, const httplib::Request&, httplib::Response& res) {
    json fonts = json::array();
    for (const auto& f : ctx.fonts->list_infos())
        fonts.push_back({{"id", f.id}, {"name", f.name}, {"file_name", f.file_name},
                         {"default", f.is_default}, {"builtin", f.builtin}});
    json j = {{"default", ctx.fonts->default_id()}, {"fonts", fonts}};
    res.set_content(j.dump(), "application/json");
}

// GET /api/v1/render/fonts/{font_id}/file
// 与 CPython os.stat().st_mtime 完全一致的 mtime（秒，double）。
//
// Starlette 的 FileResponse 用 `md5(f"{st_mtime}-{st_size}")` 当 ETag，其中 st_mtime
// 是 Python 的 repr（最短往返）。CPython 在 Windows 上算的是
// `(double)sec + (double)nsec / 1e9`（nsec 来自 FILETIME 的 100ns 单位），
// 而不是 `ft100/1e7 - 11644473600`（两者在部分文件上差 1 ulp，repr 就完全不同）。
// 用 std::filesystem::last_write_time + clock_cast 得到的值同样对不齐：
// 实测 arialbd.ttf 的 ETag 因此与 Python 不同（客户端无法复用字体缓存）。
static double file_mtime_for_etag(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &info))
        return 0.0;
    const unsigned long long ft100 =
        (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    // CPython timemodule.c 的算法：以 1601→1970 的差值做无符号减法，再拆成
    // sec + nsec。旧的 `ft100*100 - 11644473600*1e9` 中间值超出 int64 范围
    // （C4307 / 有符号溢出 UB），这里全程 unsigned 后再取结果。
    constexpr unsigned long long kEpochDiff100ns = 11644473600ULL * 10000000ULL;
    const unsigned long long delta = ft100 - kEpochDiff100ns;
    const long long sec = static_cast<long long>(delta / 10000000ULL);
    const long long nsec = static_cast<long long>(delta % 10000000ULL) * 100LL;
    return static_cast<double>(sec) + static_cast<double>(nsec) / 1e9;
}

// Starlette set_stat_headers 的 Last-Modified：
// email.utils.formatdate(st_mtime, usegmt=True) → "%a, %d %b %Y %H:%M:%S GMT"
std::string http_date_utc(double epoch_s) {
    time_t t = static_cast<time_t>(std::floor(epoch_s));
    struct tm tmv;
    gmtime_s(&tmv, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    return buf;
}

// CPython repr(float) 等价格式化（字体 ETag 需要与 Python 逐字相同）。
// 先用 std::to_chars 取最短往返十进制（与 Python 3.1+ 的 repr 同为 shortest
// round-trip），再按 Python 的显示规则重排：仅当十进制指数 < -4 或 >= 16 用
// 科学计数法（指数至少两位），否则定点并在整数值后补 ".0"。
// nlohmann 的 dump 走 %g 式阈值（1500000000.0 会输出 "1.5e+09"，Python repr 是
// "1500000000.0"），md5 输入一旦不同，字体缓存就无法复用（审查 #7）。
std::string py_repr_double(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    const bool neg = std::signbit(v);
    const double a = neg ? -v : v;
    char buf[64];
    std::string s;
    const auto res = std::to_chars(buf, buf + sizeof(buf), a, std::chars_format::scientific);
    if (res.ec == std::errc()) {
        s.assign(buf, static_cast<size_t>(res.ptr - buf));
    } else {
        // 兜底：MSVC/C++17 起 to_chars 浮点一定可用，这里只防意外
        const int n = std::snprintf(buf, sizeof(buf), "%.17e", a);
        if (n > 0)
            s.assign(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                               : sizeof(buf) - 1);
    }
    const size_t epos = s.find('e');
    if (epos == std::string::npos || epos == 0) return s;  // 理论上不可达
    std::string digits;
    for (size_t i = 0; i < epos; i++)
        if (s[i] != '.') digits.push_back(s[i]);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int e = 0;
    bool eneg = false;
    size_t p = epos + 1;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) eneg = s[p++] == '-';
    for (; p < s.size(); p++) e = e * 10 + (s[p] - '0');
    if (eneg) e = -e;
    std::string out;
    if (e < -4 || e >= 16) {  // Python repr 的指数阈值（decpt <= -4 || decpt > 16）
        out.push_back(digits[0]);
        if (digits.size() > 1) {
            out.push_back('.');
            out += digits.substr(1);
        }
        char eb[8];
        std::snprintf(eb, sizeof(eb), "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        out += eb;
    } else if (e >= 0) {
        const size_t ip = static_cast<size_t>(e) + 1;
        if (ip >= digits.size())
            out = digits + std::string(ip - digits.size(), '0') + ".0";
        else
            out = digits.substr(0, ip) + "." + digits.substr(ip);
    } else {
        out = "0." + std::string(static_cast<size_t>(-e - 1), '0') + digits;
    }
    return neg ? "-" + out : out;
}

void handle_font_file(AppContext& ctx, const httplib::Request& req, httplib::Response& res) {
    const std::string font_id = req.matches[1];
    auto path = ctx.fonts->face_path(font_id);
    if (path.empty())
        throw AppError(ErrorCode::AssetNotFound, "字体不存在",
                       json::object({{"font_id", font_id}}));
    const std::string ext = ext_of(path.string());
    const std::string mime = (ext == ".otf" || ext == ".otc") ? "font/otf" : "font/ttf";
    res.set_header("Content-Disposition",
                   "attachment; filename=\"" + path.filename().string() + "\"");
    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
    // Starlette 的 FileResponse 会自动补 ETag / Accept-Ranges（responses.py:321-341）。
    // 缺了它们客户端无法对字体做条件请求，每次 JSON 模式对齐排版都要重下整个字体。
    // ETag 公式与 Starlette 一致：md5("{st_mtime repr}-{st_size}")，浮点必须用
    // py_repr_double 生成与 Python repr 逐字相同的文本（nlohmann 的 dump 会分叉）。
    std::error_code ec;
    const auto st_size = std::filesystem::file_size(path, ec);
    if (!ec) {
        // mtime 只取一次，ETag 与 Last-Modified 共用
        const double mtime = file_mtime_for_etag(path);
        if (mtime != 0.0) {
            const std::string etag =
                "\"" + md5_hex(py_repr_double(mtime) + "-" +
                               std::to_string(static_cast<long long>(st_size))) +
                "\"";
            // Last-Modified 与 Starlette 一致；If-Modified-Since 与 Python 一样不处理
            res.set_header("Last-Modified", http_date_utc(mtime));
            res.set_header("ETag", etag);
            // 与其它端点一致的 If-None-Match 语义（*、W/ 前缀、逗号列表）
            if (ctx.etag_matches(inm_ptr(req), etag)) { res.status = 304; return; }
        }
    }
    // 条件请求未命中才读整份字体：避免 304 也全量读盘+分配（审查建议）
    std::string bytes;
    if (!read_file_bytes(path, bytes))
        throw AppError(ErrorCode::AssetNotFound, "字体不存在",
                       json::object({{"font_id", font_id}}));
    // 与 Python 相同：文件可读即声明支持 Range（不依赖 ETag 是否算得出来）
    res.set_header("Accept-Ranges", "bytes");
    const long long total = static_cast<long long>(bytes.size());
    if (req.has_header("Range")) {
        // 与 pages 的 Range 语义共用同一解析器（§4.6）：语法无效 → 忽略按 200；
        // 越界 → 416 + Content-Range；单段命中 → 206。
        RangeSpec r;
        const RangeKind kind = parse_range(req.get_header_value("Range"), total, r);
        if (kind == RangeKind::Unsatisfiable) {
            res.status = 416;
            res.set_header("Content-Range", r.content_range);
            return;
        }
        if (kind == RangeKind::Ok) {
            res.status = 206;
            res.set_header("Content-Range", r.content_range);
            res.set_content(bytes.substr(static_cast<size_t>(r.start),
                                         static_cast<size_t>(r.end - r.start + 1)),
                            mime);
            return;
        }
    }
    res.set_content(bytes, mime);
}

// GET / 与 GET /healthz（鉴权，无前缀）
void handle_root(AppContext& ctx, const httplib::Request&, httplib::Response& res) {
    json j = {{"name", ctx.cfg.server.name}, {"version", "0.1.0"},
              {"api", "/api/v1"}, {"auth_required", ctx.cfg.auth_required()}};
    res.set_content(j.dump(), "application/json");
}

void handle_healthz(AppContext& ctx, const httplib::Request&, httplib::Response& res) {
    json j = {{"ok", true}, {"scanning", ctx.library->scanning()}};
    res.set_content(j.dump(), "application/json");
}

// 未命中任何路由时的兜底。httplib 默认回一个非 JSON 的 404 空响应，而 Python 走
// Starlette：路径不存在 → 404（code BAD_PATH，message "Not Found"）；
// 路径存在但方法不对 → 405（code BAD_REQUEST，message "Method Not Allowed"）。
// 客户端按 code 分支，且这两条都是可观测契约（见 PORT_SPEC §1.2 补充映射）。
void handle_unmatched(const httplib::Request& req, httplib::Response& res) {
    // 正则预编译为函数级 static（C++11 起首次初始化线程安全）：此前每个兜底请求
    // 现场构造 2×17 个 std::regex（两个循环各一次），扫描期间 HTTP 风暴下纯浪费。
    struct Route {
        std::regex re;
        const char* method;
    };
    static const std::vector<Route> kRoutes = {
        {std::regex(R"(/api/v1/server/info)"), "GET"},
        {std::regex(R"(/api/v1/files)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/meta)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/cover)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/asset)"), "GET"},
        {std::regex(R"(/api/v1/books/open-folder)"), "POST"},
        {std::regex(R"(/api/v1/books/[^/]+/layout)"), "POST"},
        {std::regex(R"(/api/v1/books/[^/]+/pages/[^/]+)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/pages/[^/]+/meta)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/locate)"), "GET"},
        {std::regex(R"(/api/v1/books/[^/]+/search)"), "GET"},
        {std::regex(R"(/api/v1/progress/[^/]+)"), "GET"},
        {std::regex(R"(/api/v1/progress/[^/]+)"), "PUT"},
        {std::regex(R"(/api/v1/render/fonts)"), "GET"},
        {std::regex(R"(/api/v1/render/fonts/[^/]+/file)"), "GET"},
        {std::regex(R"(/)"), "GET"},
        {std::regex(R"(/healthz)"), "GET"},
    };
    bool path_known = false;
    for (const auto& r : kRoutes) {
        if (!std::regex_match(req.path, r.re)) continue;
        path_known = true;
        if (req.method == r.method) return;  // 理论上不可达（真实路由已注册）
    }
    // 尾斜杠：Starlette 的 redirect_slashes 会 307 到无斜杠路径（审查 L14）
    if (!req.path.empty() && req.path.back() == '/' && req.path.size() > 1) {
        std::string trimmed = req.path.substr(0, req.path.size() - 1);
        for (const auto& r : kRoutes) {
            if (!std::regex_match(trimmed, r.re)) continue;
            if (req.method != r.method) continue;
            res.status = 307;
            res.set_header("Location", trimmed);
            return;
        }
    }
    if (path_known)
        throw AppError(ErrorCode::BadRequest, "Method Not Allowed", nullptr, 405);
    throw AppError(ErrorCode::BadPath, "Not Found", nullptr, 404);
}

}  // namespace

// ---------------------------------------------------------------- 路由注册
std::unique_ptr<httplib::Server> create_server(AppContext& ctx) {
    auto svr = std::make_unique<httplib::Server>();
    // 线程池 12（与 Python 每请求在线程池跑 CPU 活的模型对应）。
    // 只用单参数形式：ThreadPool(n, max_n, mqr) 的第二/第三参数（线程上限 / 排队上限）
    // 在本项目的实测里会引入问题（12 基准线程 + 8 上限 → 堆损坏 0xc0000374；
    // 12/12/1024 同样在并发压力下崩），保持上游默认行为最稳。
    svr->new_task_queue = [] { return new httplib::ThreadPool(12); };

    // httplib 自身产生的错误（URI/头部超长 → 414、请求行非法 → 400）默认是**空 body**，
    // 客户端按 JSON 解析会直接失败。这里统一补上错误信封；已有 body（我们自己的
    // 信封）原样返回（审查 M10）。
    svr->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        // 已有 body（我们自己的错误信封）→ 原样返回。
        // X-Page-Index/Content-Range 存在说明这是 handle_page / handle_font_file
        // 自己产出的 Range 响应：越界时 Python 返回的是**无 body、无 content-type**
        // 的 416 + Content-Range（routes_read.py:306-310），补信封会让客户端/对拍
        // 看到多余字段。
        if (!res.body.empty() || res.has_header("X-Page-Index") ||
            res.has_header("Content-Range"))
            return httplib::Server::HandlerResponse::Unhandled;
        ErrorCode code = ErrorCode::BadRequest;
        std::string msg = "请求参数缺失或非法";
        if (res.status == 404) { code = ErrorCode::BadPath; msg = "Not Found"; }
        else if (res.status == 401) { code = ErrorCode::Unauthorized; msg = "令牌缺失或错误"; }
        else if (res.status == 403) { code = ErrorCode::PathTraversal; msg = "越出书库根目录"; }
        else if (res.status == 405) { msg = "Method Not Allowed"; }
        else if (res.status == 414) { msg = "请求 URI 过长"; }
        else if (res.status == 416) { msg = "请求范围不合法"; }
        else if (res.status >= 500) { code = ErrorCode::Internal; msg = "服务器内部错误"; }
        res.set_content(error_body(code, msg).dump(), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });

    // GET /api/v1/server/info：唯一免鉴权端点
    svr->Get(R"(/api/v1/server/info)", wrap(ctx, false,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_server_info(ctx, req, res); }));

    // ---- 以下全部鉴权 ----
    svr->Get(R"(/api/v1/files)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_files(ctx, req, res); }));

    svr->Get(R"(/api/v1/books/([^/]+)/meta)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_book_meta(ctx, req, res); }));
    svr->Get(R"(/api/v1/books/([^/]+)/cover)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_cover(ctx, req, res); }));
    svr->Get(R"(/api/v1/books/([^/]+)/asset)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_asset(ctx, req, res); }));
    svr->Post(R"(/api/v1/books/open-folder)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_open_folder(ctx, req, res); }));

    svr->Post(R"(/api/v1/books/([^/]+)/layout)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_layout(ctx, req, res); }));
    // 页码段用 ([^/]+) 而不是 (\d+)：Python 的路径参数是 int，非数字会走校验失败
    // → 400 BAD_REQUEST + detail.errors（loc ["path","page"]），而 (\d+) 不匹配
    // 会变成 404，两边状态码不一致。
    svr->Get(R"(/api/v1/books/([^/]+)/pages/([^/]+))", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_page(ctx, req, res); }));
    svr->Get(R"(/api/v1/books/([^/]+)/pages/([^/]+)/meta)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_page_meta(ctx, req, res); }));
    svr->Get(R"(/api/v1/books/([^/]+)/locate)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_locate(ctx, req, res); }));
    svr->Get(R"(/api/v1/books/([^/]+)/search)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_search(ctx, req, res); }));

    svr->Get(R"(/api/v1/progress/([^/]+))", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_progress_get(ctx, req, res); }));
    svr->Put(R"(/api/v1/progress/([^/]+))", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_progress_put(ctx, req, res); }));

    svr->Get(R"(/api/v1/render/fonts)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_fonts(ctx, req, res); }));
    svr->Get(R"(/api/v1/render/fonts/([^/]+)/file)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_font_file(ctx, req, res); }));

    svr->Get(R"(/)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_root(ctx, req, res); }));
    svr->Get(R"(/healthz)", wrap(ctx, true,
        [&ctx](const httplib::Request& req, httplib::Response& res) { handle_healthz(ctx, req, res); }));

    // 兜底：未命中上面的路由 → 404/405（不鉴权，与 Starlette 一致：
    // 路由匹配发生在依赖求解之前，未知路径不会先报 401）。
    // HEAD 交给 handle_unmatched 判定：未知路径 → 404（与 Python 相同）。
    auto fallback = wrap(ctx, false, [](const httplib::Request& req, httplib::Response& res) {
        handle_unmatched(req, res);
    }, /*head_405=*/false);
    svr->Get(R"(.*)", fallback);
    svr->Post(R"(.*)", fallback);
    svr->Put(R"(.*)", fallback);
    svr->Delete(R"(.*)", fallback);
    svr->Options(R"(.*)", fallback);
    svr->Patch(R"(.*)", fallback);

    return svr;
}

int run_http(AppContext& ctx, const std::string& host, int port) {
    auto svr = create_server(ctx);
    // 先清标志再发布指针：反过来的话，落在这两步之间的 Ctrl+C 会被清掉，
    // 且 listen 失败时会被误报成"监听失败"（审查 L12）
    g_stop_requested = false;
    g_server = svr.get();
    if (!svr->listen(host, port)) {   // listen 失败（含 stop() 打断）
        g_server = nullptr;
        if (!g_stop_requested) {
            std::fprintf(stderr, "监听失败：%s:%d\n", host.c_str(), port);
            return 1;
        }
        return 0;                     // Ctrl 优雅退出
    }
    g_server = nullptr;
    return 0;
}

void request_http_stop() {
    g_stop_requested = true;
    if (auto* s = g_server.load()) s->stop();   // 通知 listen 循环退出
}

}  // namespace lnv
