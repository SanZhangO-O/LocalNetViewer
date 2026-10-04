#include "lnv/context.h"
#include "lnv/auth.h"
#include "lnv/formats/registry.h"
#include "lnv/netinfo.h"

namespace lnv {

std::unique_ptr<AppContext> AppContext::create(AppConfig config, bool background_scan) {
    std::unique_ptr<AppContext> ctx(new AppContext());
    ctx->cfg = std::move(config);
    ensure_dirs(ctx->cfg);

    ctx->fonts = std::make_unique<FontManager>();
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
    ctx->layouts = std::make_unique<LayoutsService>(ctx->cfg, *ctx->fonts,
                                                    *ctx->layouts_cache, *ctx->parsed_cache);

    ctx->page_cache = std::make_unique<LruCache>(ctx->cfg.cache.page_lru_bytes,
                                                 ctx->cfg.cache.page_lru_pages);
    ctx->cover_cache = std::make_unique<LruCache>(64LL * 1024 * 1024, 256);
    ctx->gate = nullptr;  // 解析闸门由 services.cpp 的进程级 ParseGate 承担

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
    if (progress) progress->flush();
}

std::optional<BookRef> AppContext::require_book(const std::string& book_id) const {
    // 图集虚拟书优先（不在 library 索引里，注册表见 cache/galleries.json）
    auto ref = gallery->book_ref(book_id);
    if (!ref) ref = library->require_book(book_id);
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
    layouts_cache->invalidate_book(book_id);
    std::error_code ec;
    std::filesystem::remove(cfg.parsed_cache_dir() / (book_id + ".1.json.gz"), ec);
    auto contains = [&](const std::string& key) {
        return key.find(book_id) != std::string::npos;
    };
    cover_cache->remove_if(contains);
    page_cache->remove_if(contains);
    layout_memory.remove_if(contains);
    doc_memory.remove_if(contains);
}

std::optional<LayoutEntry> AppContext::require_layout(const std::string& book_id,
                                                      const std::string& layout_id) {
    std::string key = book_id + ":" + layout_id;
    if (auto e = layout_memory.get(key)) {
        return e;
    }
    auto entry = layouts_cache->get(book_id, layout_id);
    if (!entry) {
        throw AppError(ErrorCode::LayoutNotFound,
                       "layout_id 不存在或已失效，请重新 POST /layout",
                       nlohmann::json{{"book_id", book_id}, {"layout_id", layout_id}});
    }
    layout_memory.put(key, *entry);
    return entry;
}

std::unique_ptr<BookAdapter> AppContext::open_adapter(const BookRef& ref) {
    return create_adapter(cfg, ref);
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

std::string AppContext::page_etag(const std::string& layout_id, int page,
                                  const std::string& theme, double scale, const std::string& fit,
                                  std::optional<int> quality, bool pair, bool rtl, bool fixed) {
    std::vector<std::string> parts = {layout_id, fixed ? "-" : theme, scale_tag_of(scale),
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
                                       int page, const std::string& theme, double scale,
                                       const std::string& fit, std::optional<int> quality,
                                       bool pair, bool rtl, bool fixed) {
    std::vector<std::string> parts = {book_id, layout_id, std::to_string(page),
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
    for (const auto& lib : library->libraries_info())
        out += "书库: " + lib.name + " (" + std::to_string(lib.book_count) + " 本)\n";
    out += "字体: " + fonts->default_id() + "（共 " +
           std::to_string(fonts->list_infos().size()) + " 个）\n";
    out += cfg.auth_required() ? "访问令牌: 已启用\n" : "访问令牌: 未启用\n";
    return out;
}

}  // namespace lnv
