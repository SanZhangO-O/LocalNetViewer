// 图集服务 / 布局服务 / 进度 / 搜索实现
// （对应 Python services/gallery.py、services/layouts.py、services/progress.py、services/search.py）
#include "lnv/services/services.h"

#include <cmath>

#include "lnv/formats/container.h"
#include "lnv/formats/registry.h"
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

// 图集 rev 记忆 TTL：dir_mtime 未变时最多复用 2s，兼顾"recursive 子目录增删的
// 可见延迟"与"每请求全量枚举"的开销（对齐 Python GALLERY_MEMO_TTL_S）。
constexpr long long kGalleryMemoTtlMs = 2000;

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

// Python str.isprintable() 的码点判定：Cc/Cf/Cs/Co/Zl/Zp 与除空格外所有 Zs
// 均为不可打印。Cn（未分配码点）需要完整 Unicode 表，这里只覆盖非字符区，
// 其余按可打印处理——device_id 是外部输入，差异面仅限于未分配码点。
bool is_printable_cp(char32_t c) {
    if (c == U' ') return true;                                  // 唯一的可打印 Zs
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return false;      // Cc
    if (c >= 0xD800 && c <= 0xDFFF) return false;                // Cs（代理区）
    if ((c & 0xFFFEu) == 0xFFFEu) return false;                  // 各平面 FFFE/FFFF 非字符
    if (c >= 0xFDD0 && c <= 0xFDEF) return false;                // 非字符
    if (c >= 0xE000 && c <= 0xF8FF) return false;                // Co（BMP 私用区）
    if (c >= 0xF0000 && c <= 0x10FFFD) return false;             // Co/Cn（15/16 平面私用区）
    static const struct { char32_t lo, hi; } kNonPrintable[] = {
        {0x00A0, 0x00A0}, {0x00AD, 0x00AD}, {0x0600, 0x0605}, {0x061C, 0x061C},
        {0x06DD, 0x06DD}, {0x070F, 0x070F}, {0x0890, 0x0891}, {0x08E2, 0x08E2},
        {0x1680, 0x1680}, {0x180E, 0x180E}, {0x2000, 0x200A}, {0x200B, 0x200F},
        {0x2028, 0x2029}, {0x202A, 0x202E}, {0x202F, 0x202F}, {0x205F, 0x205F},
        {0x2060, 0x2064}, {0x2066, 0x206F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF},
        {0xFFF9, 0xFFFB}, {0x110BD, 0x110BD}, {0x110CD, 0x110CD},
        {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A},
        {0xE0001, 0xE0001}, {0xE0020, 0xE007F},
    };
    for (const auto& r : kNonPrintable) {
        if (c >= r.lo && c <= r.hi) return false;
    }
    return true;
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

// file_time → epoch 秒（图集目录 mtime 用；library.cpp 同款实现）
double file_time_to_epoch_local(const std::filesystem::file_time_type& t) {
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::duration<double>>(sys.time_since_epoch())
        .count();
}

// 图集路径自身的 mtime（对齐 Python gallery.resolve/register 的 real.stat()：
// 单图图集 stat 的就是图片文件本身，不是父目录）；stat 失败返回 nullopt →
// rev 退回 book_id（跨实现契约）
std::optional<double> path_mtime_opt(const std::filesystem::path& real) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(real, ec);
    if (ec) return std::nullopt;
    return file_time_to_epoch_local(t);
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
    // 重新加载注册表后旧计数可能已失真：整体作废 rev 记忆
    invalidate_rev_memo();
}

void GalleryService::save_registry() {
    // 快照与写盘必须在同一把锁内（Python gallery.py save_registry 在 RLock 内完成）：
    // 分离时并发 register 的快照可能后于他人写盘落盘，把已注册的条目覆盖回旧状态，
    // galleries.json 丢失注册（book_id 是哈希，无法反推路径）。
    std::unique_lock lk(mu_);
    nlohmann::json data = nlohmann::json::object();
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
    std::vector<std::string> pruned_ids;
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
        // 注册表上限（与 Python gallery.MAX_REGISTRY 一致）：每次 open-folder 都可能
        // 新增一条，不设限则 galleries.json 随浏览过的目录数无限增长、整文件重写
        // 耗时 O(n) 上涨。按最近更新时间淘汰；被淘汰 id 的 rev 记忆在锁外作废。
        constexpr size_t kMaxRegistry = 5000;
        if (registry_.size() > kMaxRegistry) {
            std::vector<std::pair<double, std::string>> order;
            order.reserve(registry_.size());
            for (auto& [bid, ent] : registry_) order.push_back({ent.updated_at, bid});
            std::sort(order.begin(), order.end());
            size_t overflow = registry_.size() - kMaxRegistry;
            for (size_t i = 0; i < overflow; i++) {
                registry_.erase(order[i].second);
                pruned_ids.push_back(order[i].second);
            }
        }
    }
    for (auto& bid : pruned_ids) invalidate_rev_memo(bid);
    save_registry();
    // 本次注册用了自己的实时计数：作废该书可能残留的旧 rev 记忆
    invalidate_rev_memo(book_id);

    GalleryInfoResult out;
    out.book_id = book_id;
    out.title = title;
    out.format = "gallery";
    out.page_count = static_cast<int>(images.size());
    // cover_url 带当前 rev（跨实现契约）：客户端在内容变化后能拿到新 URL。
    // 对齐 Python register：图片数用本次实时枚举结果、mtime 用 real.stat()，
    // stat 失败退化为 book_id。
    auto dir_mtime = path_mtime_opt(real);
    const std::string rev = dir_mtime
                                ? compute_gallery_rev(book_id, out.page_count, dir_mtime)
                                : book_id;
    out.cover_url = "/api/v1/books/" + book_id + "/cover?rev=" + rev;
    return out;
}

void GalleryService::invalidate_rev_memo(const std::string& book_id) {
    std::lock_guard<std::mutex> lk(rev_memo_mu_);
    if (book_id.empty()) {
        rev_memo_.clear();
    } else {
        rev_memo_.erase(book_id);
    }
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
    // 图集修订号（对齐 Python gallery.resolve）：路径 stat 失败退回 book_id；
    // 否则需要图片数——recursive 模式下目录 mtime 不随子目录增删变化，只有
    // 实时计数能反映内容变化。但枚举很贵且 book_ref() 每请求都调：目录 mtime
    // 未变且在 TTL 内的重复请求复用记忆计数；mtime 一变立即重算（注册/重载时
    // 还会显式作废）。单图图集计 1（与 ComicAdapter 的单页一致）。
    auto dir_mtime = path_mtime_opt(real);
    if (!dir_mtime) {
        ref.rev = book_id;
    } else {
        const double dm = *dir_mtime;
        int image_count = 0;
        std::string revision;
        bool memo_hit = false;
        {
            std::lock_guard<std::mutex> lk(rev_memo_mu_);
            auto mit = rev_memo_.find(book_id);
            if (mit != rev_memo_.end() && mit->second.dir_mtime &&
                std::fabs(*mit->second.dir_mtime - dm) < 1e-6 &&
                (std::chrono::steady_clock::now() - mit->second.checked_at) <
                    std::chrono::milliseconds(kGalleryMemoTtlMs)) {
                image_count = mit->second.image_count;
                revision = mit->second.revision;
                memo_hit = true;
            }
        }
        if (!memo_hit) {
            std::error_code sec;
            image_count = std::filesystem::is_regular_file(real, sec)
                              ? 1
                              : static_cast<int>(list_images(real, e.recursive).size());
            revision = compute_gallery_rev(book_id, image_count, dir_mtime);
            std::lock_guard<std::mutex> lk(rev_memo_mu_);
            RevMemo m;
            m.dir_mtime = dm;
            m.image_count = image_count;
            m.revision = revision;
            m.checked_at = std::chrono::steady_clock::now();
            rev_memo_[book_id] = std::move(m);
        }
        ref.rev = revision;
    }
    // extra["dir"]：真实路径（UTF-8 字符串），适配器据此枚举图片
    ref.extra["dir"] = real.u8string();
    ref.extra["recursive"] = e.recursive;
    ref.extra["title"] = e.title;
    return ref;
}

std::vector<std::filesystem::path> GalleryService::list_images(
    const std::filesystem::path& dir, bool recursive) const {
    // 与目录容器走同一条枚举路径（open_dir → DirContainer → list_dir_image_names）：
    // 图集 rev 计数与适配器取页共用一份带缓存的目录枚举，不再各自重扫目录，也不
    // 逐文件 stat（对应 Python gallery.list_images → comics.list_folder_images）。
    // 返回绝对路径（供 register 计数/标题与 book_ref 计数）。
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    auto container = open_dir(dir, recursive, cfg_.comic.skip_entries);
    const auto& entries = container->entries();
    std::vector<std::filesystem::path> out;
    out.reserve(entries.size());
    for (const auto& e : entries) out.push_back(dir / std::filesystem::u8path(e.name));
    return out;
}

// ================================================================== LayoutsService

// 按适配器能力修正 mode（逐行对齐 Python effective_profile）：
// - flow 适配器强制 flow；
// - fixed 适配器请求 flow → fixed；请求 strip 仅漫画/图集能力支持，否则 → fixed
//   （不是 415：固定版式对 strip 的语义是退回 fixed）。
// strip_overlap_px 仅在修正后仍是 strip 时补配置默认值。
static LayoutProfile effective_profile(const AppConfig& cfg, BookAdapter& adapter,
                                       const LayoutProfile& profile) {
    LayoutProfile p = profile;
    if (adapter.mode == "flow") {
        if (p.mode != "flow") p.mode = "flow";  // flow 适配器强制 flow
        return p;
    }
    if (p.mode == "flow") p.mode = "fixed";
    if (p.mode == "strip" && !adapter.supports_strip()) p.mode = "fixed";
    if (p.mode == "strip" && cfg.comic.strip_max_source_images > 0) {
        // strip 要按视口高度切片，必须逐张探测源图高度。图集上万张时 /layout 会
        // 变成数万次读图（客户端等到超时）。超过上限回退 fixed：一图一页，只枚举
        // 页集，设备端按需取"当前页 + 预取页"。对齐 Python layouts.effective_profile。
        int count = 0;
        try {
            count = adapter.page_count();
        } catch (...) {
            count = 0;
        }
        if (count > cfg.comic.strip_max_source_images) p.mode = "fixed";
    }
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
// 所有超时任务（解析、分页）共用同一个 8 槽上限。
// 故意不析构：超时任务可能在 main 返回后仍在跑，静态析构会让 worker 的
// guard.release() 访问已销毁的 mutex；进程退出时由 OS 回收（Python 的 _RUNNER
// 同样是模块级对象、活到解释器结束）。
static ParseGate& parse_gate() {
    static ParseGate* gate = new ParseGate();
    return *gate;
}

void wait_parse_tasks_idle(double timeout_s) {
    parse_gate().wait_idle(std::chrono::milliseconds(
        static_cast<long long>(std::max(0.0, timeout_s) * 1000.0)));
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
    std::string font_id = fonts_->resolve_id(prof.font.family);
    std::string layout_id = compute_layout_key(prof, font_id);

    if (prof.mode == "flow") {
        // 三级缓存：内存 → 磁盘 → 实际解析 + 分页
        auto cached = entry(ref.book_id, layout_id);
        if (cached) {
            return make_result(cached->pagemap, layout_id, nullptr, true);
        }
        // 同一 (book, layout) 的首次分页串行化（Python LayoutService._locks 的 KeyedLocks）：
        // 后到者**阻塞等待**前者的结果，再查缓存返回 cache_hit=true。直接 503 是
        // Python 没有的可观测失败；等锁期间不占闸门槽位，槽位留给真正在跑的重活。
        auto layout_lock = layout_locks_.acquire("layout:" + ref.book_id + ":" + layout_id);
        if (auto again = entry(ref.book_id, layout_id)) {
            return make_result(again->pagemap, layout_id, nullptr, true);
        }
        const double parse_timeout = cfg_.limits.parse_timeout_s;
        std::shared_ptr<const DocumentModel> doc =
            std::make_shared<DocumentModel>(document_internal(ref, "解析分页"));
        // 分页同样受超时保护（Python：run_with_timeout(lambda: engine.build(...),
        // key=("layout", …))）。worker 持有 DocumentModel 的 shared_ptr 与
        // profile/font_id 副本：调用方超时返回后它们仍然有效；超时的分页继续在后台跑，
        // 槽位等它真正结束才释放（不强杀线程）。
        LayoutProfile prof_copy = prof;
        std::string fid = font_id;
        PageMap pm = run_with_timeout(
            parse_gate(),
            // fonts 按值捕获 shared_ptr：超时分离的 worker 即使活过 AppContext
            // 也能保证 FontManager 存活（审查 H：悬垂 FontManager）。
            [doc, prof_copy, fid, fonts = fonts_]() {
                FlowLayoutEngine::Deps d;
                d.doc = doc.get();
                d.fonts = fonts.get();
                d.profile = &prof_copy;
                d.font_id = fid;
                FlowLayoutEngine engine(d);
                return engine.build();  // 页数超限内部抛 ParseError
            },
            parse_timeout, "分页", "layout:" + ref.book_id + ":" + layout_id);
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
        return make_result(e.pagemap, layout_id, doc.get(), false);
    }

    // 固定版式 / 条漫：页集合每次实时枚举（不缓存页内容，设计 4.6）。
    // 枚举本身可能是重活（strip 要探测每张图原始尺寸，CBR 要列目录），与 flow
    // 同等对待：同一 (book_id, layout_id) 加锁避免并发重复枚举 + run_with_timeout
    // 超时保护（超时 → PARSE_FAILED）。worker 必须自持配置/引用并自行打开适配器：
    // 超时返回后调用方会立刻销毁传入的 adapter，捕获引用会让后台线程悬垂。
    auto layout_lock = layout_locks_.acquire("layout:" + ref.book_id + ":" + layout_id);
    const double parse_timeout = cfg_.limits.parse_timeout_s;
    AppConfig cfg_copy = cfg_;
    BookRef ref_copy = ref;
    LayoutProfile prof_copy = prof;
    PageMap pm = run_with_timeout(
        parse_gate(),
        [cfg = std::move(cfg_copy), bref = std::move(ref_copy), prof_w = prof_copy,
         fonts = fonts_]() {
            PageMap out;
            out.book_id = bref.book_id;
            out.mode = prof_w.mode;
            out.created_at = 0.0;  // 与 Python 一致（见上）
            auto own = [&] {
                auto heavy = heavy_gate().acquire();  // 适配器构造可能是重活
                AdapterEnv env;
                env.fonts = fonts.get();
                return create_adapter(cfg, bref, env);
            }();
            BookAdapter& ad = *own;
            if (prof_w.mode == "strip") {
                auto slices = ad.strip_pages(prof_w.avail_height(), prof_w.strip_overlap_px);
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
                    out.pages.push_back(std::move(rec));
                }
                if (!slices.empty()) {
                    const StripPage& last = slices.back();
                    out.end_anchor[0] = 0;
                    out.end_anchor[1] = last.image_index;
                    out.end_anchor[2] = static_cast<int>(last.y0 + last.height);
                } else {
                    out.end_anchor[0] = out.end_anchor[1] = out.end_anchor[2] = 0;
                }
            } else {
                int total = ad.page_count();
                for (int i = 0; i < std::max(0, total); i++) {
                    PageRecord rec;
                    rec.page = i + 1;
                    rec.anchor[0] = 0;
                    rec.anchor[1] = i;  // 对齐 Python：第 i+1 页锚点 (0, i, 0)
                    rec.anchor[2] = 0;
                    out.pages.push_back(std::move(rec));
                }
                out.end_anchor[0] = 0;
                out.end_anchor[1] = std::max(0, total);
                out.end_anchor[2] = 0;
            }
            out.total_pages = static_cast<int>(out.pages.size());
            return out;
        },
        parse_timeout, "分页", "layout:" + ref.book_id + ":" + layout_id);
    pm.layout_id = layout_id;
    if (pm.book_id.empty()) pm.book_id = ref.book_id;
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
    // 与 Python LayoutStore.get 一致：并发从磁盘加载同一 layout 时保留**先写入内存**的
    // 权威副本；put_if_absent 同时把命中的条目移到最新（LRU 语义不变）
    if (auto existing = memory_.put_if_absent(key, *e)) return existing;
    return e;
}

DocumentModel LayoutsService::document(const BookRef& ref, BookAdapter& adapter) {
    // Python 的 document() 用调用方传入的 adapter；C++ 的实际解析由超时 worker 执行，
    // 而分离的 worker 无法安全使用调用方栈上、超时返回即析构的 adapter（悬垂指针），
    // 因此 worker 自行打开等价 adapter（同一 cfg+ref，结果一致）。参数保留以兼容签名。
    (void)adapter;
    return document_internal(ref, "解析");
}

DocumentModel LayoutsService::document_internal(const BookRef& ref, const std::string& what) {
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        auto hit = doc_memory_.get(ref.book_id);
        if (hit) return *hit;
    }
    // 同一本书的解析串行化（Python ParsedCache._locks）：后到者阻塞等待前者写入
    // 内存/磁盘，冷书不会被 N 个请求同时全量解析。等锁期间不占闸门槽位。
    auto parse_lock = parse_locks_.acquire("parse:" + ref.book_id);
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        auto hit = doc_memory_.get(ref.book_id);
        if (hit) return *hit;
    }
    auto doc = parsed_.get(ref.book_id);  // 磁盘解析缓存（损坏自动删除）
    if (!doc) {
        // 解析放进超时 runner（Python run_with_timeout，key=("parse", book_id)）：
        // 超时 → 422 PARSE_FAILED + reason=timeout；任务继续在后台跑，槽位等它结束才
        // 释放。worker 自持 cfg/ref 副本并自行打开 adapter，超时返回后不会悬垂。
        AppConfig cfg_copy = cfg_;
        BookRef ref_copy = ref;
        doc = run_with_timeout(
            parse_gate(),
            // fonts 按值捕获 shared_ptr（同上）；env 让内层适配器（EPUB 文字封面）
            // 使用 AppContext 的同一个 FontManager。
            [cfg = std::move(cfg_copy), bref = std::move(ref_copy), fonts = fonts_]() {
                auto own = [&] {
                    auto heavy = heavy_gate().acquire();  // 适配器构造可能是重活（MOBI 转换）
                    AdapterEnv env;
                    env.fonts = fonts.get();
                    return create_adapter(cfg, bref, env);
                }();
                return own->document();
            },
            cfg_.limits.parse_timeout_s, what, "parse:" + ref.book_id);
        parsed_.put(*doc);
    }
    {
        std::lock_guard<std::shared_mutex> lk(mu_);
        doc_memory_.put(ref.book_id, *doc);
    }
    return *doc;
}

void LayoutsService::invalidate_book(const std::string& book_id) {
    // Python context.invalidate_book_caches 会清 LayoutStore 的内存 PageMap 与
    // ParsedCache 的热文档：少了这一步，内容变更后旧分页会一直服务到进程重启。
    std::string prefix = book_id + ":";
    std::lock_guard<std::shared_mutex> lk(mu_);
    memory_.remove_if([&](const std::string& key) { return key.rfind(prefix, 0) == 0; });
    doc_memory_.remove_if([&](const std::string& key) { return key == book_id; });
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

namespace {
// json 数值 → ordered_json：nlohmann 不提供跨 object_t 类型的隐式转换，
// 外层必须用 ordered_json 保持 Python 的字段序。
nlohmann::ordered_json num_to_ordered(const nlohmann::json& v) {
    if (v.is_number_unsigned()) return nlohmann::ordered_json(v.get<unsigned long long>());
    if (v.is_number_integer()) return nlohmann::ordered_json(v.get<long long>());
    if (v.is_number_float()) return nlohmann::ordered_json(v.get<double>());
    return nlohmann::ordered_json(nullptr);
}
}  // namespace

// record → 落盘/外发 JSON（updated_at 为 ISO 字符串；updated_at_ts 仅落盘不外发）
nlohmann::ordered_json ProgressService::Record::to_public_json() const {
    // device_id / page / ratio 可空；updated_at 为 ISO 字符串（updated_at_ts 不外发）。
    // ordered_json：字段序与 Python put() 构造 dict 的顺序一致（book_id, device_id,
    // anchor, page, ratio, updated_at），普通 json 会按键名排序导致落盘字节不同。
    nlohmann::ordered_json jdev =
        device_id ? nlohmann::ordered_json(*device_id) : nlohmann::ordered_json(nullptr);
    nlohmann::ordered_json jpage =
        page ? num_to_ordered(*page) : nlohmann::ordered_json(nullptr);
    nlohmann::ordered_json jratio =
        ratio ? nlohmann::ordered_json(*ratio) : nlohmann::ordered_json(nullptr);
    return {
        {"book_id", book_id},
        {"device_id", jdev},
        {"anchor",
         {{"chapter", num_to_ordered(anchor[0])},
          {"block", num_to_ordered(anchor[1])},
          {"char", num_to_ordered(anchor[2])}}},
        {"page", jpage},
        {"ratio", jratio},
        {"updated_at", iso_time_utc(updated_at_ts)},
    };
}

std::string ProgressService::normalize_device(const std::string& device) {
    // strip → 空 → "default" → 剔除 Unicode 不可打印字符与 :"\/ → 截断 64 字符。
    // 只按 ASCII 空白 strip 与 Python str.strip() 等价：会被 Python strip 掉的
    // 空白/控制码点都是不可打印字符，随后也会被打印性过滤删除，最终串一致。
    size_t b = device.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "default";
    size_t e = device.find_last_not_of(" \t\r\n\f\v");
    U32Text cps = utf8_to_u32(device.substr(b, e - b + 1));
    U32Text cleaned;
    for (char32_t c : cps) {
        if (!is_printable_cp(c) || c == U':' || c == U'"' || c == U'/' || c == U'\\')
            continue;
        cleaned.push_back(c);
    }
    if (cleaned.size() > 64) cleaned.resize(64);
    if (cleaned.empty()) return "default";
    return u32_to_utf8(cleaned);
}

void ProgressService::load() {
    std::string raw;
    if (!read_all(file_, raw)) return;
    nlohmann::ordered_json data;  // 保留文件内的键序（Python dict 顺序）
    try {
        data = nlohmann::ordered_json::parse(raw);
    } catch (...) {
        return;
    }
    if (!data.is_object()) return;
    std::unique_lock lk(mu_);
    records_.clear();
    order_seq_ = 0;
    for (auto& [key, val] : data.items()) {
        if (!val.is_object()) continue;
        Record r;
        r.book_id = val.value("book_id", std::string());
        if (val.contains("device_id") && val["device_id"].is_string())
            r.device_id = val["device_id"].get<std::string>();
        if (val.contains("anchor") && val["anchor"].is_object()) {
            // 原样保留整数数值（含 2^63..2^64-1）；浮点按 Python int() 的截断语义归一。
            // 注意 val 是 ordered_json，跨类型赋值要显式取数值。
            static constexpr const char* kNames[3] = {"chapter", "block", "char"};
            const auto& a = val["anchor"];
            for (int i = 0; i < 3; ++i) {
                if (!a.contains(kNames[i])) continue;
                const auto& n = a[kNames[i]];
                if (n.is_number_unsigned())
                    r.anchor[i] = n.get<unsigned long long>();
                else if (n.is_number_integer())
                    r.anchor[i] = n.get<long long>();
                else if (n.is_number_float())
                    r.anchor[i] = static_cast<long long>(n.get<double>());
            }
        }
        if (val.contains("page") && val["page"].is_number()) {
            const auto& n = val["page"];
            if (n.is_number_unsigned())
                r.page = n.get<unsigned long long>();
            else if (n.is_number_integer())
                r.page = n.get<long long>();
            else
                r.page = static_cast<long long>(n.get<double>());
        }
        if (val.contains("ratio") && val["ratio"].is_number())
            r.ratio = val["ratio"].get<double>();
        r.updated_at_ts = val.value("updated_at_ts", 0.0);
        r.order = ++order_seq_;
        records_[key] = std::move(r);
    }
}

void ProgressService::flush() {
    {
        std::shared_lock lk(mu_);
        if (!dirty_) return;  // 没有未落盘改动就不写（Python flush 的 dirty 检查）
    }
    std::lock_guard<std::mutex> fg(flush_mu_);  // 串行化：并发落盘会互相覆盖
    save_locked(/*force=*/true);
}

// 调用方必须持有 flush_mu_。节流判定、快照与写盘完成后的 dirty 处理都在锁下，
// last_flush_/dirty_/generation_ 不会成为竞态（审查 M6）。
bool ProgressService::save_locked(bool force) {
    double now = now_epoch();
    uint64_t gen = 0;
    std::string payload;
    {
        std::unique_lock lk(mu_);
        if (!force && now - last_flush_ < 1.0) {
            dirty_ = true;  // 有改动但还在合并窗口内，等下次 put / 关停 flush
            return false;
        }
        payload = dump_locked();
        last_flush_ = now;
        generation_++;
        gen = generation_;
    }
    bool ok = atomic_write(file_, payload);
    {
        std::unique_lock lk(mu_);
        if (ok) {
            // 写盘期间没有新写入（代数未变）才清 dirty：否则那段新数据还没落盘，
            // 清掉会让关停 flush 直接跳过（Python save 的 generation 语义）
            if (generation_ == gen) dirty_ = false;
        } else {
            dirty_ = true;  // 落盘失败保留 dirty，下次重试
        }
    }
    return ok;
}

// 调用方必须持有 mu_。按插入序落盘并用 indent=1：
// 与 Python json.dumps(self._data, ensure_ascii=False, indent=1) 逐字节一致。
std::string ProgressService::dump_locked() const {
    std::vector<const std::pair<const std::string, Record>*> ordered;
    ordered.reserve(records_.size());
    for (auto& kv : records_) ordered.push_back(&kv);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* a, const auto* b) { return a->second.order < b->second.order; });
    nlohmann::ordered_json data = nlohmann::ordered_json::object();
    for (const auto* kv : ordered) {
        nlohmann::ordered_json j = kv->second.to_public_json();
        j["updated_at_ts"] = kv->second.updated_at_ts;  // 内部字段，不外发但落盘
        data[kv->first] = std::move(j);
    }
    return data.dump(1);
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
                                             const nlohmann::json anchor[3],
                                             std::optional<nlohmann::json> page,
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
        std::string key = book_id + ":" + dev;
        auto it = records_.find(key);
        // Python dict 语义：覆盖已有 key 不改变插入位置，新 key 追加到末尾
        r.order = it != records_.end() ? it->second.order : ++order_seq_;
        records_[key] = r;
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
        dirty_ = true;
        generation_++;  // 与 save 的快照代数配对：写盘期间的更新不能被误清 dirty
    }
    // 落盘必须持 flush_mu_（save_locked 契约；锁序 flush_mu_ → mu_，与 flush() 一致）：
    // 否则并发 put 可能旧快照后落盘覆盖新记录
    std::lock_guard<std::mutex> fg(flush_mu_);
    // 写入节流 ≥1s：间隔内仅保留内存态，由下次 put / 关停 flush 落盘
    save_locked(/*force=*/false);
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
