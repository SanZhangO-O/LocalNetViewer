#include "lnv/context.h"
#include "lnv/auth.h"
#include "lnv/formats/registry.h"
#include "lnv/netinfo.h"
#include "lnv/util/paths.h"

namespace lnv {

std::unique_ptr<AppContext> AppContext::create(AppConfig config, bool background_scan) {
    std::unique_ptr<AppContext> ctx(new AppContext());
    ctx->cfg = std::move(config);
    ensure_dirs(ctx->cfg);

    ctx->fonts = std::make_shared<FontManager>();
    ctx->fonts->init(ctx->cfg.fonts_dir(), ctx->cfg.render.default_font);

    ctx->library = std::make_unique<LibraryService>(ctx->cfg);
    ctx->library->load_index();
    ctx->gallery = std::make_unique<GalleryService>(ctx->cfg, *ctx->library);
    ctx->gallery->load_registry();

    ctx->progress = std::make_unique<ProgressService>(ctx->cfg.gallery_registry_path()
                                                          .parent_path() / "progress.json");
    ctx->progress->load();

    ctx->parsed_cache = std::make_unique<ParsedCache>(ctx->cfg.parsed_cache_dir(),
                                                      ctx->cfg.cache.parsed_max_mb);
    ctx->layouts_cache = std::make_unique<LayoutsDiskCache>(ctx->cfg.layouts_cache_dir(),
                                                            ctx->cfg.cache.layouts_max_mb);
    ctx->layouts = std::make_unique<LayoutsService>(ctx->cfg, ctx->fonts,
                                                    *ctx->layouts_cache, *ctx->parsed_cache);

    ctx->page_cache = std::make_unique<LruCache>(ctx->cfg.cache.page_lru_bytes,
                                                 ctx->cfg.cache.page_lru_pages);
    ctx->cover_cache = std::make_unique<LruCache>(64LL * 1024 * 1024, 256);
    ctx->gate = nullptr;  // 解析闸门由 services.cpp 的进程级 ParseGate 承担

    // MOBI 转换产物（cache/converted）启动时按 2GiB 配额做一次 mtime-LRU 清理
    enforce_dir_quota(ctx->cfg.cache_dir() / "converted", 2LL * 1024 * 1024 * 1024);

    // 书籍内容变化 → 丢弃全部由旧内容派生的缓存（对应 Python context.py:59）。
    // 少了这一步，"同尺寸就地编辑"会让服务器永远返回旧分页与旧页面图。
    AppContext* self = ctx.get();
    ctx->library->add_content_listener(
        [self](const std::string& book_id) { self->invalidate_book_caches(book_id); });

    if (background_scan) ctx->library->start_background_scan();
    return ctx;
}

void AppContext::shutdown() {
    if (library) {
        library->cancel_scan();
        // 先等后台扫描协作式退出再落盘：Python main.py:79-81 同样先 wait_idle(10)
        // 再 save_index，否则关停时 ~std::thread 会析构 joinable 线程 → std::terminate
        // （审查 H1），且索引可能在扫描中途被写坏。
        library->wait_idle(10.0);
        library->save_index();
    }
    // 再等解析/分页 worker 收尾（最多 10s）：它们按值捕获了 FontManager 的
    // shared_ptr、DocumentModel 的 shared_ptr 与各自的配置副本，即使超时后活过
    // AppContext 也不会悬垂；这里等一等只是让关停更干净。卡死任务放弃等待（无法强杀）。
    wait_parse_tasks_idle(10.0);
    if (progress) progress->flush();
}

std::optional<BookRef> AppContext::require_book(const std::string& book_id,
                                                const AccessScope* scope) const {
    // 图集虚拟书优先（不在 library 索引里，注册表见 cache/galleries.json）
    auto ref = gallery->book_ref(book_id);
    if (!ref) ref = library->require_book(book_id);
    // 不在访问范围内的书与不存在同样报 404：不能靠 book_id 访问其他书库的内容
    if (ref && scope != nullptr && !scope->all &&
        !scope->allows(LibraryService::root_name_of(ref->vpath))) {
        ref.reset();
    }
    if (!ref) {
        throw AppError(ErrorCode::BookNotFound, "书不存在或已被移除",
                       nlohmann::json{{"book_id", book_id}});
    }
    if (ref->virtual_book) return ref;  // 图集书无指纹校验
    // 指纹校验（size/mtime 变化 → 重算 book_id；变化则由 library 更新索引）
    auto refreshed = library->refresh_book(book_id);
    if (refreshed) return refreshed;
    return ref;
}

void AppContext::invalidate_book_caches(const std::string& book_id) {
    // 与 Python context.invalidate_book_caches 对齐：布局（磁盘 + LayoutsService 内存层）、
    // 解析（磁盘 + 热文档）、页面图与封面，以及 MOBI 转换产物。只清磁盘不清
    // LayoutsService 的内存 LRU 时，内容变更后 /layout 仍会命中旧 PageMap 到进程重启。
    // rev 由 size/mtime 现算，内容变化后自然变新（缓存键/ETag 不再命中）。
    layouts->invalidate_book(book_id);
    layouts_cache->invalidate_book(book_id);
    std::error_code ec;
    std::filesystem::remove(
        cfg.parsed_cache_dir() / (book_id + "." + kParsedCacheVersion + ".json.gz"), ec);
    // MOBI/AZW3：内容变化后旧转换产物必须丢弃（否则 /layout 仍读旧 EPUB）。
    // 产物按 revision 命名（{book_id}.{rev}.epub），新 rev 天然不命中旧文件；
    // 这里再清掉该书全部旧 rev 产物与 .tmp（含在途转换先写的临时文件），
    // 避免竞态窗口里旧产物被再次采用或堆积。
    {
        std::error_code dir_ec;
        std::filesystem::directory_iterator it(cfg.cache_dir() / "converted", dir_ec);
        const std::string prefix = book_id + ".";
        for (auto end = std::filesystem::directory_iterator(); !dir_ec && it != end;
             it.increment(dir_ec)) {
            const std::string name = path_to_utf8(it->path().filename());
            if (name.compare(0, prefix.size(), prefix) == 0) {
                std::error_code rec;
                std::filesystem::remove(it->path(), rec);
            }
        }
    }
    auto contains = [&](const std::string& key) {
        return key.find(book_id) != std::string::npos;
    };
    cover_cache->remove_if(contains);
    page_cache->remove_if(contains);
}

std::optional<LayoutEntry> AppContext::require_layout(const std::string& book_id,
                                                      const std::string& layout_id) {
    // 读穿 LayoutsService 的统一内存/磁盘缓存（与 /layout、/search 共用一套，
    // 失效路径只需清它自己的 memory_/doc_memory_）
    auto entry = layouts->entry(book_id, layout_id);
    if (!entry) {
        throw AppError(ErrorCode::LayoutNotFound,
                       "layout_id 不存在或已失效，请重新 POST /layout",
                       nlohmann::json{{"book_id", book_id}, {"layout_id", layout_id}});
    }
    return entry;
}

std::unique_ptr<BookAdapter> AppContext::open_adapter(const BookRef& ref) {
    // 重活闸门：适配器构造可能触发外部解压列表 / MOBI 转换（分钟级）。
    // 可重入，MOBI 内部再取不会重复占槽；持闸门期间不等待解析闸门。
    auto heavy = heavy_gate().acquire();
    AdapterEnv env;
    env.fonts = fonts.get();
    return create_adapter(cfg, ref, env);
}

// ---------------------------------------------------------------- HTTP 缓存
static std::string scale_tag_of(double scale) {
    // 对齐 context.py::_scale_tag：
    //   value = round(float(scale or 1.0), 2)         ← 就近取偶，不是远离零
    //   整十分位 → "%.1f"，否则 "%.2f"
    // 用 std::round 时 ?scale=0.625 会得到 0.63（Python 是 0.62），同一个 URL
    // 在两个可互换的服务器上给出不同 ETag → 客户端换服务器后缓存全部失效。
    double v = (scale == 0.0) ? 1.0 : scale;  // Python: scale or 1.0
    double value = std::nearbyint(v * 100.0) / 100.0;
    double r1 = std::nearbyint(value * 10.0) / 10.0;
    char buf[32];
    if (std::fabs(value - r1) < 1e-9)
        std::snprintf(buf, sizeof(buf), "%.1f", value);
    else
        std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

std::string AppContext::page_etag(const std::string& layout_id, const std::string& rev, int page,
                                  const std::string& theme, double scale, const std::string& fit,
                                  std::optional<int> quality, bool pair, bool rtl, bool fixed) {
    // 契约：rev 是第二段 → "<layout_id>:<rev>:<theme|->:<scale_tag>:<page>[:extras]"
    std::vector<std::string> parts = {layout_id, rev, fixed ? "-" : theme, scale_tag_of(scale),
                                      std::to_string(page)};
    if (fixed) {
        parts.push_back(fit);
        parts.push_back(std::to_string(quality ? *quality : 0));
        parts.push_back(pair ? "pair" : "-");
        parts.push_back(rtl ? "rtl" : "-");
    }
    std::string out = "\"";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out.push_back(':');
        out += parts[i];
    }
    out.push_back('"');
    return out;
}

std::string AppContext::page_cache_key(const std::string& book_id, const std::string& layout_id,
                                       const std::string& rev, int page, const std::string& theme,
                                       double scale, const std::string& fit,
                                       std::optional<int> quality, bool pair, bool rtl, bool fixed) {
    // 契约顺序：[book_id, layout_id, rev, page, theme|"", scale]（+ fixed 附加项不变）
    std::vector<std::string> parts = {book_id, layout_id, rev, std::to_string(page),
                                      fixed ? "" : theme, scale_tag_of(scale)};
    if (fixed) {
        parts.push_back(fit);
        parts.push_back(std::to_string(quality ? *quality : 0));
        parts.push_back(pair ? "1" : "0");
        parts.push_back(rtl ? "1" : "0");
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out.push_back('\x1f');
        out += parts[i];
    }
    return out;
}

bool AppContext::etag_matches(const std::string* header_value, const std::string& etag) {
    if (!header_value || header_value->empty()) return false;
    size_t b = header_value->find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    std::string v = *header_value;
    if (v == "*") return true;
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find(',', i);
        if (j == std::string::npos) j = v.size();
        std::string part = v.substr(i, j - i);
        i = j + 1;
        size_t pb = part.find_first_not_of(" \t");
        size_t pe = part.find_last_not_of(" \t");
        if (pb == std::string::npos) continue;
        part = part.substr(pb, pe - pb + 1);
        if (part.rfind("W/", 0) == 0) part = part.substr(2);
        if (part == etag) return true;
    }
    return false;
}

std::string AppContext::startup_report() const {
    std::string out = "LocalNetViewer Server (C++) v0.1.0\n";
    std::string locked;
    for (const auto& root : cfg.library.roots) {
        if (!root.password.empty()) {
            if (!locked.empty()) locked += "、";
            locked += root.name;
        }
    }
    for (const auto& lib : library->libraries_info()) {
        const LibraryRootCfg* root = cfg.root_by_name(lib.name);
        const bool needs_password = root != nullptr && !root->password.empty();
        out += "书库: " + lib.name + " (" + std::to_string(lib.book_count) + " 本)  [" +
               (needs_password ? "需密码" : "公开") + "]\n";
    }
    out += "字体: " + fonts->default_id() + "（共 " +
           std::to_string(fonts->list_infos().size()) + " 个）\n";
    out += cfg.auth_required() ? "访问令牌: 已启用\n" : "访问令牌: 未启用\n";
    if (!locked.empty())
        out += "访问密码: " + locked + "（输入对应密码只可见该书库）\n";
    return out;
}

}  // namespace lnv
