#pragma once
// 格式注册表 + 上下文（对应 Python formats/registry.py、app/context.py）
#include "lnv/cache/lru.h"
#include "lnv/cache/store.h"
#include "lnv/config.h"
#include "lnv/formats/adapter.h"
#include "lnv/layout/fonts.h"
#include "lnv/services/library.h"
#include "lnv/services/services.h"

namespace lnv {

struct AppContext {
    AppConfig cfg;
    // shared_ptr：分离的超时 worker 按值捕获，可在 AppContext 销毁后继续安全使用
    std::shared_ptr<FontManager> fonts;
    std::unique_ptr<LibraryService> library;
    std::unique_ptr<GalleryService> gallery;
    std::unique_ptr<ParsedCache> parsed_cache;
    std::unique_ptr<LayoutsDiskCache> layouts_cache;
    std::unique_ptr<LayoutsService> layouts;
    std::unique_ptr<ProgressService> progress;

    // 内存缓存（硬编码上限与 Python 一致）。布局 PageMap / 热文档内存层由
    // LayoutsService 统一持有（memory_ / doc_memory_，按 config 上限配置）：
    // AppContext 里再放一套会与它分叉，失效路径也只能清一半。
    std::unique_ptr<LruCache> page_cache;   // 512MB/512 张（config）
    std::unique_ptr<LruCache> cover_cache;  // 64MB/256 张

    // 并发解析闸门（concurrency.py 语义）：上限 8、同任务去重 → SERVER_BUSY
    class ParseGate* gate = nullptr;

    // ---- 生命周期 ----
    static std::unique_ptr<AppContext> create(AppConfig config, bool background_scan);
    void shutdown();  // 停扫描 → save index → progress flush

    // ---- 书籍 ----
    // scope 非空时校验书籍所在书库是否可见；不可见按"书不存在"抛 404（设计 9）
    std::optional<BookRef> require_book(const std::string& book_id,
                                        const AccessScope* scope = nullptr) const;
    std::optional<LayoutEntry> require_layout(const std::string& book_id,
                                              const std::string& layout_id);
    // 指纹变化时清空派生缓存
    void invalidate_book_caches(const std::string& book_id);

    // ---- 适配器 ----
    // registry.create_adapter；不支持 → UnsupportedFormatError（→415），超限 → ParseError
    std::unique_ptr<BookAdapter> open_adapter(const BookRef& ref);

    // ---- HTTP 缓存辅助（§4.6；rev = BookRef.rev 内容修订号）----
    static std::string page_etag(const std::string& layout_id, const std::string& rev, int page,
                                 const std::string& theme, double scale, const std::string& fit,
                                 std::optional<int> quality, bool pair, bool rtl, bool fixed);
    static std::string page_cache_key(const std::string& book_id, const std::string& layout_id,
                                      const std::string& rev, int page, const std::string& theme,
                                      double scale, const std::string& fit,
                                      std::optional<int> quality, bool pair, bool rtl, bool fixed);
    static bool etag_matches(const std::string* header_value, const std::string& etag);
    // 启动横幅信息（netinfo 地址枚举）
    std::string startup_report() const;
};

}  // namespace lnv
