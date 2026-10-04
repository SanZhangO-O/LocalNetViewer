// 图集服务 / 布局服务 / 进度 / 搜索实现
// （对应 Python services/gallery.py、services/layouts.py、services/progress.py、services/search.py）
#include "lnv/services/services.h"

#include <cmath>

#include "lnv/formats/container.h"
#include "lnv/util/concurrency.h"
#include "lnv/util/paths.h"
#include "lnv/util/sha1.h"
#include "lnv/util/textutil.h"

namespace lnv {
namespace { std::string u8_name(const std::filesystem::path& p) {
    auto w = p.filename().wstring();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
} }


namespace {

// ------------------------------------------------------------------ 通用小工具

// 虚拟路径 → 真实路径（含穿越校验）；根名未知返回空路径
// （library.cpp 的同名逻辑为匿名命名空间，无法跨 TU 复用，此处独立实现）
std::filesystem::path resolve_vpath_local(const AppConfig& cfg, const std::string& vpath) {
    std::string v = normalize_vpath(vpath, true);
    std::vector<std::string> parts;
    for (auto& p : vpath_parts(v)) parts.push_back(p);
    if (parts.empty()) return {};
    const LibraryRootCfg* root = cfg.root_by_name(parts[0]);
    if (root == nullptr) return {};
    return resolve_in_root(std::filesystem::u8path(root->path),
                           std::vector<std::string>(parts.begin() + 1, parts.end()));
}

// 码点级小写化（ASCII；非 ASCII 原样——搜索只对纯 ASCII 查询词做忽略大小写）
char32_t lower_cp(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + (U'a' - U'A') : c;
}

// u32 串内查找子串（返回首下标，找不到 npos）
size_t u32_find(const U32Text& hay, const U32Text& needle, size_t from) {
    if (needle.empty()) return from;
    if (needle.size() > hay.size()) return U32Text::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        while (j < needle.size() && hay[i + j] == needle[j]) j++;
        if (j == needle.size()) return i;
    }
    return U32Text::npos;
}

// 读整个文件（文本/二进制均可）
bool read_all(const std::filesystem::path& p, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

// ================================================================== GalleryService

void GalleryService::load_registry() {
    std::string raw;
    if (!read_all(cfg_.gallery_registry_path(), raw)) {
        return;  // 不存在 → 空注册表
    }
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(raw);
    } catch (...) {
        return;  // 损坏 → 清空（与 Python 一致）
    }
    if (!data.is_object()) return;
    std::unique_lock lk(mu_);
    registry_.clear();
    for (auto& [book_id, val] : data.items()) {
        if (!val.is_object()) continue;
        Entry e;
        e.path = val.value("path", std::string());
        e.recursive = val.value("recursive", false);
        e.title = val.value("title", std::string());
        e.page_count = val.value("page_count", 0);
        e.created_at = val.value("created_at", 0.0);
        e.updated_at = val.value("updated_at", 0.0);
        registry_[book_id] = std::move(e);
    }
}

void GalleryService::save_registry() {
    nlohmann::json data = nlohmann::json::object();
    {
        std::shared_lock lk(mu_);
        for (auto& [book_id, e] : registry_) {
            data[book_id] = {
                {"path", e.path},
                {"recursive", e.recursive},
                {"title", e.title},
                {"page_count", e.page_count},
                {"created_at", e.created_at},
                {"updated_at", e.updated_at},
            };
        }
    }
    // dump(1)：ensure_ascii=false + indent=1，与 Python save_registry 一致
    atomic_write(cfg_.gallery_registry_path(), data.dump(1));
}

GalleryService::GalleryInfoResult GalleryService::register_folder(
    const std::string& vpath_raw, std::optional<bool> recursive) {
    std::string v;
    try {
        v = normalize_vpath(vpath_raw);  // 不允许空 → PathError → BadPath
    } catch (const PathError& e) {
        throw AppError(ErrorCode::BadPath, e.what());
    }
    std::filesystem::path real;
    try {
        real = resolve_vpath_local(cfg_, v);
    } catch (const PathError& e) {
        throw AppError(ErrorCode::BadPath, e.what());
    } catch (const TraversalError& e) {
        throw AppError(ErrorCode::PathTraversal, e.what());
    }
    std::error_code ec;
    if (real.empty() || !std::filesystem::exists(real, ec))
        throw AppError(ErrorCode::BadPath, "路径不存在: " + v);

    bool rec = recursive.value_or(cfg_.gallery.recursive);

    std::vector<std::filesystem::path> images;
    std::string title;
    if (std::filesystem::is_regular_file(real)) {
        if (!is_image_ext(ext_of(u8_name(real)))) {
            // Python gallery.py:150 带 reason="非图片文件"（detail 是契约的一部分，审查 M）
            throw AppError(ErrorCode::UnsupportedFormat, "该文件不是图片，无法作为图集打开",
                           nlohmann::json{{"reason", "非图片文件"}});
        }
        images.push_back(real);
        title = stem_of(u8_name(real));
        rec = false;  // 单图片文件 → 单页图集
    } else {
        for (auto& p : list_images(real, rec)) images.push_back(p);
        std::string dirname = u8_name(real);
        title = dirname.empty() ? v.substr(v.rfind('/') + 1) : dirname;
    }

    int min_images = std::max(1, cfg_.gallery.min_images_in_folder);
    if (static_cast<int>(images.size()) < min_images) {
        throw AppError(ErrorCode::UnsupportedFormat,
                       "目录内图片数不足（" + std::to_string(images.size()) + " < " +
                           std::to_string(min_images) + "），无法作为图集打开");
    }

    std::string book_id = stable_hash("gallery:" + v, 16);
    {
        std::unique_lock lk(mu_);
        Entry e;
        auto pit = registry_.find(book_id);
        if (pit != registry_.end()) e.created_at = pit->second.created_at;
        if (e.created_at == 0) e.created_at = now_epoch();
        e.path = v;
        e.recursive = rec;
        e.title = !title.empty() ? title
                                 : (pit != registry_.end() && !pit->second.title.empty()
                                        ? pit->second.title
                                        : v.substr(v.rfind('/') + 1));
        e.page_count = static_cast<int>(images.size());
        e.updated_at = now_epoch();
        registry_[book_id] = std::move(e);
    }
    save_registry();

    GalleryInfoResult out;
    out.book_id = book_id;
    out.title = title;
    out.format = "gallery";
    out.page_count = static_cast<int>(images.size());
    out.cover_url = "/api/v1/books/" + book_id + "/cover";
    return out;
}

std::optional<BookRef> GalleryService::book_ref(const std::string& book_id) const {
    Entry e;
    {
        std::shared_lock lk(mu_);
        auto it = registry_.find(book_id);
        if (it == registry_.end()) return std::nullopt;
        e = it->second;
    }
    std::string vpath;
    try {
        vpath = normalize_vpath(e.path, true);
    } catch (const PathError&) {
        return std::nullopt;
    }
    std::filesystem::path real;
    try {
        real = resolve_vpath_local(cfg_, vpath);
    } catch (...) {
        return std::nullopt;
    }
    std::error_code ec;
    if (real.empty() || !std::filesystem::exists(real, ec)) return std::nullopt;
    BookRef ref;
    ref.book_id = book_id;
    ref.vpath = vpath;
    ref.path = real;
    ref.format = "gallery";
    ref.size = 0;
    ref.mtime = 0.0;
    ref.virtual_book = true;
    // extra["dir"]：真实路径（UTF-8 字符串），适配器据此枚举图片
    ref.extra["dir"] = real.u8string();
    ref.extra["recursive"] = e.recursive;
    ref.extra["title"] = e.title;
    return ref;
}

std::vector<std::filesystem::path> GalleryService::list_images(
    const std::filesystem::path& dir, bool recursive) const {
    // 对应 Python gallery.list_images：跳过隐藏与 skip_entries glob、
    // 符号链接不计；返回绝对路径，按相对路径自然排序。
    std::vector<std::pair<std::string, std::filesystem::path>> found;
    const auto& patterns = cfg_.comic.skip_entries;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};

    auto consider = [&](const std::string& rel, const std::filesystem::path& abs) {
        if (should_skip_entry(rel, patterns)) return;
        if (!is_image_ext(ext_of(rel))) return;
        found.emplace_back(rel, abs);
    };

    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::error_code ec2;
            if (it->is_symlink(ec2) || ec2) { ec2.clear(); continue; }
            if (!it->is_regular_file(ec2) || ec2) { ec2.clear(); continue; }
            std::string name = u8_name(it->path());
            if (is_hidden_name(name)) continue;
            // rel 路径（UTF-8，'/' 分隔）
            std::string rel = it->path().lexically_relative(dir).generic_string();
            consider(rel, it->path());
        }
    } else {
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code ec2;
            if (it->is_symlink(ec2) || ec2) { ec2.clear(); continue; }
            if (!it->is_regular_file(ec2) || ec2) { ec2.clear(); continue; }
            std::string name = u8_name(it->path());
            if (is_hidden_name(name)) continue;
            consider(name, it->path());
        }
    }

    // 自然排序（按 rel 文本；非递归时 rel = 文件名）：2.jpg < 10.jpg
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return natural_less_u32(utf8_to_u32(a.first), utf8_to_u32(b.first));
    });

    std::vector<std::filesystem::path> out;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(f.second);
    return out;
}

// ================================================================== LayoutsService

// 按适配器能力修正 mode（对应 Python effective_profile；C++ 无 ComicAdapter
// 类型可判，任务约定：fixed 适配器请求 strip 一律按 strip 处理）
static LayoutProfile effective_profile(const AppConfig& cfg, const BookAdapter& adapter,
                                       const LayoutProfile& profile) {
    LayoutProfile p = profile;
    if (adapter.mode == "flow") {
        if (p.mode != "flow") p.mode = "flow";  // flow 适配器强制 flow
        return p;
    }
    // fixed 适配器：请求 flow → fixed；请求 strip → strip
    if (p.mode == "flow") p.mode = "fixed";
    if (p.mode == "strip" && p.strip_overlap_px == 0 && cfg.comic.strip_overlap_px > 0) {
        p.strip_overlap_px = cfg.comic.strip_overlap_px;
    }
    return p;
}

static std::vector<nlohmann::json> chapters_json(const PageMap& pm,
                                                 const DocumentModel* doc) {
    // ChapterInfo JSON：{index,title,total_chars?,start_page?}；缺省键输出 null
    std::vector<nlohmann::json> out;
    out.reserve(pm.chapters.size());
    for (const auto& ch : pm.chapters) {
        nlohmann::json total = nullptr;
        if (ch.total_chars) {
            total = *ch.total_chars;
        } else if (doc) {
            // flow：total_chars 从 DocumentModel 章节的 total_chars 取
            for (const auto& dc : doc->chapters) {
                if (dc.index == ch.index) {
                    total = static_cast<long long>(dc.total_chars());
                    break;
                }
            }
        }
        nlohmann::json start = ch.start_page ? nlohmann::json(*ch.start_page)
                                             : nlohmann::json(nullptr);
        out.push_back({
            {"index", ch.index},
            {"title", u32_to_utf8(ch.title)},
            {"total_chars", total},
            {"start_page", start},
        });
    }
    return out;
}

// 进程级解析闸门（一个进程只有一个 AppContext，等价于 Python 的模块级 _TimedRunner）
static ParseGate& parse_gate() {
    static ParseGate gate;
    return gate;
}

// LayoutBuildResult 组装（对应 Python LayoutService._result）
static LayoutBuildResult make_result(const PageMap& pm, const std::string& layout_id,
                                     const DocumentModel* doc, bool cache_hit) {    LayoutBuildResult r;
    r.layout_id = layout_id;
    r.mode = pm.mode;
    r.total_pages = pm.total_pages;
    r.chapters = chapters_json(pm, doc);
    r.cache_hit = cache_hit;
    r.strip = (pm.mode == "strip");
    return r;
}

LayoutBuildResult LayoutsService::build(const BookRef& ref, BookAdapter& adapter,
                                        const LayoutProfile& profile) {
    LayoutProfile prof = effective_profile(cfg_, adapter, profile);
    std::string font_id = fonts_.resolve_id(prof.font.family);
    std::string layout_id = compute_layout_key(prof, font_id);

    if (prof.mode == "flow") {
        // 三级缓存：内存 → 磁盘 → 实际解析 + 分页
        auto cached = entry(ref.book_id, layout_id);
        if (cached) {
            return make_result(cached->pagemap, layout_id, nullptr, true);
        }
        // 并发闸门（对应 Python layouts.py:128-142 的 run_with_timeout + _TimedRunner）：
        // 同一个 (book, layout) 的重复请求 → 503 SERVER_BUSY（去重，不重复跑重活）；
        // 同时在跑的解析任务达上限 → 503。没有闸门时，12 个线程可以同时为同一本书
        // 做全量解析与分页，把整台机器拖死。
        // 顺序很重要：**先** try_acquire 拿到槽位，**再**武装 RAII Guard。
        // Guard 析构会无条件 release()（concurrency.h），若先武装再 acquire，
        // 被 503 拒绝的请求会顺手删掉正在跑任务的去重键、并释放一个自己没占的槽位
        // ——去重与"并发上限 8"会同时失效（审查 H1）。
        const std::string gate_key = "layout:" + ref.book_id + ":" + layout_id;
        parse_gate().try_acquire(gate_key);   // 失败抛 ServerBusyError，此时 Guard 尚未武装
        ParseGate::Guard gate_guard(parse_gate(), gate_key);
        // 等闸门期间别的线程可能已经算完 → 再查一次缓存
        if (auto again = entry(ref.book_id, layout_id)) {
            return make_result(again->pagemap, layout_id, nullptr, true);
        }
        DocumentModel doc = document(ref, adapter);
        FlowLayoutEngine::Deps d;
        d.doc = &doc;
        d.fonts = &fonts_;
        d.profile = &prof;
        d.font_id = font_id;
        FlowLayoutEngine engine(d);
        PageMap pm = engine.build();  // 页数超限内部抛 ParseError
        pm.layout_id = layout_id;
        // created_at 保持 0.0：Python engine.py:647 构造 PageMap 时恒写 0.0，
        // 这里写真实时间戳会让共享 layouts/*.json.gz 出现无意义差异（审查 L16）
        pm.created_at = 0.0;
        if (pm.book_id.empty()) pm.book_id = ref.book_id;
        LayoutEntry e{prof, std::move(pm)};
        disk_.put(ref.book_id, e);
        {
            std::lock_guard<std::shared_mutex> lk(mu_);
            memory_.put(ref.book_id + ":" + layout_id, e);
        }
        return make_result(e.pagemap, layout_id, &doc, false);
    }

    // 固定版式 / 条漫：页集合每次实时枚举（不缓存页内容）
    PageMap pm;
    pm.book_id = ref.book_id;
    pm.layout_id = layout_id;
    pm.mode = prof.mode;
    pm.created_at = 0.0;  // 与 Python 一致（见上）
    if (prof.mode == "strip") {
        auto slices = adapter.strip_pages(prof.avail_height(), prof.strip_overlap_px);
        for (size_t i = 0; i < slices.size(); i++) {
            const StripPage& sp = slices[i];
            PageRecord rec;
            rec.page = static_cast<int>(i) + 1;
            rec.anchor[0] = 0;
            rec.anchor[1] = sp.image_index;
            rec.anchor[2] = static_cast<int>(sp.y0);
            rec.chapter_index = 0;
            rec.image_index = sp.image_index;
            rec.y0 = static_cast<double>(sp.y0);
            rec.height = static_cast<double>(sp.height);
            pm.pages.push_back(std::move(rec));
        }
        if (!slices.empty()) {
            const StripPage& last = slices.back();
            pm.end_anchor[0] = 0;
            pm.end_anchor[1] = last.image_index;
            pm.end_anchor[2] = static_cast<int>(last.y0 + last.height);
        } else {
            pm.end_anchor[0] = pm.end_anchor[1] = pm.end_anchor[2] = 0;
        }
    } else {
        int total = adapter.page_count();
        for (int i = 0; i < std::max(0, total); i++) {
            PageRecord rec;
            rec.page = i + 1;
            rec.anchor[0] = 0;
            rec.anchor[1] = i;  // 对齐 Python：第 i+1 页锚点 (0, i, 0)
            rec.anchor[2] = 0;
            pm.pages.push_back(std::move(rec));
        }
        pm.end_anchor[0] = 0;
        pm.end_anchor[1] = std::max(0, total);
        pm.end_anchor[2] = 0;
    }
    pm.total_pages = static_cast<int>(pm.pages.size());
    LayoutEntry e{prof, std::move(pm)};
    disk_.put(ref.book_id, e);
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        memory_.put(ref.book_id + ":" + layout_id, e);
    }
    return make_result(e.pagemap, layout_id, nullptr, false);
}

std::optional<LayoutEntry> LayoutsService::entry(const std::string& book_id,
                                                 const std::string& layout_id) {
    if (layout_id.empty()) return std::nullopt;
    std::string key = book_id + ":" + layout_id;
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        auto hit = memory_.get(key);
        if (hit) return hit;
    }
    // 磁盘读取（gzip + JSON，可能数十毫秒）必须在锁外，避免串行化所有请求
    auto e = disk_.get(book_id, layout_id);
    if (!e) return std::nullopt;
    std::lock_guard<std::shared_mutex> lk(mu_);
    memory_.put(key, *e);  // 并发加载同一 layout 时以先写入内存的为准（语义可接受）
    return e;
}

DocumentModel LayoutsService::document(const BookRef& ref, BookAdapter& adapter) {
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        auto hit = doc_memory_.get(ref.book_id);
        if (hit) return *hit;
    }
    auto doc = parsed_.get(ref.book_id);  // 磁盘解析缓存（损坏自动删除）
    if (!doc) {
        doc = adapter.document();  // 实际解析（超时保护由调用方/上层负责）
        parsed_.put(*doc);
    }
    std::lock_guard<std::shared_mutex> lk(mu_);
    doc_memory_.put(ref.book_id, *doc);
    return *doc;
}

std::optional<int> LayoutsService::page_of_anchor(const PageMap& pm, const int target[3]) {
    // 对应 Python layout/anchors.page_of_anchor 的二分规则（只返回页号）：
    // 锚点单调递增 → 找"最后一个 anchor <= target"的页
    const auto& pages = pm.pages;
    if (pages.empty()) return std::nullopt;
    auto lt = [](const int a[3], const int b[3]) {  // a < b（字典序）
        for (int i = 0; i < 3; i++) {
            if (a[i] != b[i]) return a[i] < b[i];
        }
        return false;
    };
    if (lt(target, pages.front().anchor)) return pages.front().page;  // target < 首页锚
    if (!lt(target, pm.end_anchor)) return pages.back().page;         // target >= end_anchor
    size_t lo = 0, hi = pages.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (!lt(target, pages[mid].anchor))  // anchor <= target
            lo = mid;
        else
            hi = mid - 1;
    }
    return pages[lo].page;
}

// ================================================================== ProgressService

// record → 落盘/外发 JSON（updated_at 为 ISO 字符串；updated_at_ts 仅落盘不外发）
nlohmann::json ProgressService::Record::to_public_json() const {
    // device_id / page / ratio 可空；updated_at 为 ISO 字符串（updated_at_ts 不外发）
    nlohmann::json jdev = device_id ? nlohmann::json(*device_id) : nlohmann::json(nullptr);
    nlohmann::json jpage = page ? nlohmann::json(*page) : nlohmann::json(nullptr);
    nlohmann::json jratio = ratio ? nlohmann::json(*ratio) : nlohmann::json(nullptr);
    return {
        {"book_id", book_id},
        {"device_id", jdev},
        {"anchor",
         {{"chapter", anchor[0]}, {"block", anchor[1]}, {"char", anchor[2]}}},
        {"page", jpage},
        {"ratio", jratio},
        {"updated_at", iso_time_utc(updated_at_ts)},
    };
}

std::string ProgressService::normalize_device(const std::string& device) {
    // strip → 空 → "default" → 剔除不可打印(iscntrl)与 :"\/ → 截断 64 字符
    size_t b = device.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "default";
    size_t e = device.find_last_not_of(" \t\r\n\f\v");
    U32Text cps = utf8_to_u32(device.substr(b, e - b + 1));
    U32Text cleaned;
    for (char32_t c : cps) {
        bool cntrl = c < 0x20 || c == 0x7F;
        if (cntrl || c == U':' || c == U'"' || c == U'/' || c == U'\\') continue;
        cleaned.push_back(c);
    }
    if (cleaned.size() > 64) cleaned.resize(64);
    if (cleaned.empty()) return "default";
    return u32_to_utf8(cleaned);
}

void ProgressService::load() {
    std::string raw;
    if (!read_all(file_, raw)) return;
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(raw);
    } catch (...) {
        return;
    }
    if (!data.is_object()) return;
    std::unique_lock lk(mu_);
    records_.clear();
    for (auto& [key, val] : data.items()) {
        if (!val.is_object()) continue;
        Record r;
        r.book_id = val.value("book_id", std::string());
        if (val.contains("device_id") && val["device_id"].is_string())
            r.device_id = val["device_id"].get<std::string>();
        if (val.contains("anchor") && val["anchor"].is_object()) {
            r.anchor[0] = val["anchor"].value("chapter", 0LL);
            r.anchor[1] = val["anchor"].value("block", 0LL);
            r.anchor[2] = val["anchor"].value("char", 0LL);
        }
        if (val.contains("page") && val["page"].is_number()) r.page = val["page"].get<long long>();
        if (val.contains("ratio") && val["ratio"].is_number())
            r.ratio = val["ratio"].get<double>();
        r.updated_at_ts = val.value("updated_at_ts", 0.0);
        records_[key] = std::move(r);
    }
}

void ProgressService::flush() {
    std::lock_guard<std::mutex> fg(flush_mu_);  // 串行化：并发落盘会互相覆盖
    flush_locked();
}

// 调用方必须持有 flush_mu_（节流判定与落盘在同一把锁下，last_flush_ 不再是竞态）
void ProgressService::flush_locked() {
    // 整文件重写（节流由 put 控制；关停时调用 flush 强制落盘）
    nlohmann::json data = nlohmann::json::object();
    {
        std::shared_lock lk(mu_);
        for (auto& [key, r] : records_) {
            nlohmann::json j = r.to_public_json();
            j["updated_at_ts"] = r.updated_at_ts;  // 内部字段，不外发但落盘
            data[key] = j;
        }
    }
    atomic_write(file_, data.dump());  // 紧凑 dump
    last_flush_ = now_epoch();
}

std::optional<ProgressService::Record> ProgressService::get(const std::string& book_id,
                                                            const std::string& device) const {
    std::string key = book_id + ":" + normalize_device(device);
    std::shared_lock lk(mu_);
    auto it = records_.find(key);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

std::optional<ProgressService::Record> ProgressService::get_any(
    const std::string& book_id) const {
    // 未指定设备时返回该书 updated_at_ts 最新一条
    std::shared_lock lk(mu_);
    std::string prefix = book_id + ":";
    const Record* best = nullptr;
    for (auto& [key, r] : records_) {
        if (key.rfind(prefix, 0) != 0) continue;
        if (!best || r.updated_at_ts > best->updated_at_ts) best = &r;
    }
    if (!best) return std::nullopt;
    return *best;
}

ProgressService::Record ProgressService::put(const std::string& book_id,
                                             const std::optional<std::string>& device,
                                             const long long anchor[3],
                                             std::optional<long long> page,
                                             std::optional<double> ratio) {
    Record r;
    r.book_id = book_id;
    std::string dev = normalize_device(device.value_or(""));
    r.device_id = dev;
    r.anchor[0] = anchor[0];
    r.anchor[1] = anchor[1];
    r.anchor[2] = anchor[2];
    r.page = page;
    if (ratio) r.ratio = std::clamp(*ratio, 0.0, 1.0);  // clamp [0,1]
    r.updated_at_ts = now_epoch();

    {
        std::unique_lock lk(mu_);
        records_[book_id + ":" + dev] = r;
        // 上限 5000 条：按 updated_at_ts 淘汰最旧
        if (records_.size() > 5000) {
            std::vector<std::pair<double, const std::string*>> ordered;
            ordered.reserve(records_.size());
            for (auto& [k, v] : records_) ordered.push_back({v.updated_at_ts, &k});
            std::sort(ordered.begin(), ordered.end(),
                      [](auto& a, auto& b) { return a.first < b.first; });
            size_t overflow = records_.size() - 5000;
            for (size_t i = 0; i < overflow; i++) records_.erase(*ordered[i].second);
        }
    }
    // 写入节流 ≥1s：间隔内仅保留内存态，由下次 put / 关停 flush 落盘。
    // 判定与落盘都在 flush_mu_ 下完成（否则两个线程可能各自判定"该落盘"，
    // 旧快照后写覆盖新记录，审查 M6）。
    double now = now_epoch();
    {
        std::lock_guard<std::mutex> fg(flush_mu_);
        if (now - last_flush_ >= 1.0) flush_locked();
    }
    return r;
}

// ================================================================== SearchService

nlohmann::json SearchService::search(const DocumentModel& doc, const std::string& query,
                                     int limit, const PageMap* pagemap) {
    // §4.10：在 DocumentModel 上逐章逐文本块子串查找；
    // 纯 ASCII 查询词忽略大小写（偏移不变），其它精确匹配；扫描上限 500 命中。
    constexpr int kMaxScanHits = 500;
    std::string q_trim = query;
    // 去除首尾 ASCII 空白（Python str.strip 的近似）
    size_t b = q_trim.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) {
        return {{"query", ""}, {"total", 0}, {"results", nlohmann::json::array()},
                {"limited", false}};
    }
    size_t e = q_trim.find_last_not_of(" \t\r\n\f\v");
    q_trim = q_trim.substr(b, e - b + 1);

    limit = std::clamp(limit, 1, 500);
    U32Text q = utf8_to_u32(q_trim);

    bool case_insensitive = true;
    for (char32_t c : q) {
        if (c >= 128) {  // 非纯 ASCII → 精确匹配
            case_insensitive = false;
            break;
        }
    }
    U32Text needle = q;
    if (case_insensitive) {
        for (auto& c : needle) c = lower_cp(c);
    }

    nlohmann::json results = nlohmann::json::array();
    long long total = 0;
    bool truncated = false;

    for (const auto& chapter : doc.chapters) {
        if (truncated) break;
        for (size_t block_index = 0; block_index < chapter.blocks.size(); block_index++) {
            const Block& blk = chapter.blocks[block_index];
            // 仅文本块（heading/paragraph）
            if (blk.type != BlockType::Heading && blk.type != BlockType::Paragraph) continue;
            U32Text text = blk.text();
            if (text.empty()) continue;
            U32Text hay = text;
            if (case_insensitive) {
                // ASCII 串 lower 后长度不变，偏移安全
                for (auto& c : hay) c = lower_cp(c);
            }
            size_t start = 0;
            while (true) {
                size_t idx = u32_find(hay, needle, start);
                if (idx == U32Text::npos) break;
                total++;
                if (static_cast<int>(results.size()) < limit) {
                    nlohmann::json page = nullptr;
                    if (pagemap != nullptr && !pagemap->pages.empty()) {
                        int target[3] = {chapter.index, static_cast<int>(block_index),
                                         static_cast<int>(idx)};
                        auto pg = LayoutsService::page_of_anchor(*pagemap, target);
                        if (pg) page = *pg;
                    }
                    results.push_back({
                        {"chapter_index", chapter.index},
                        {"chapter_title", u32_to_utf8(chapter.title)},
                        {"block", static_cast<int>(block_index)},
                        {"char", static_cast<int>(idx)},
                        {"page", page},
                        {"snippet", u32_to_utf8(snippet_around(text, idx, idx + needle.size()))},
                    });
                }
                start = idx + std::max<size_t>(1, needle.size());
                if (total >= kMaxScanHits) {
                    truncated = true;
                    break;
                }
            }
            if (truncated) break;
        }
    }

    bool limited = truncated || total > static_cast<long long>(results.size());
    return {
        {"query", q_trim},
        {"total", total},
        {"results", results},
        {"limited", limited},
    };
}

}  // namespace lnv
