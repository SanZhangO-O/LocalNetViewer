#pragma once
// 图集服务 / 布局服务 / 进度 / 搜索（对应 Python services/{gallery,layouts,progress,search}.py）
#include <cstdint>
#include <shared_mutex>
#include "lnv/cache/lru.h"   // 【最小新增】：ObjectLru（原文件已用而未包含）
#include "lnv/cache/store.h"
#include "lnv/formats/adapter.h"
#include "lnv/layout/engine.h"
#include "lnv/services/library.h"
#include "lnv/util/concurrency.h"   // KeyedLocks / run_with_timeout（解析闸门）

namespace lnv {

// ------------------------------------------------------------------ gallery
class GalleryService {
public:
    GalleryService(const AppConfig& cfg, LibraryService& lib)
        : cfg_(cfg), lib_(lib) {}

    // cache/galleries.json
    void load_registry();
    void save_registry();

    // open-folder 注册；失败抛 AppError（415/400/404 语义按 Python）
    struct GalleryInfoResult {
        std::string book_id, title, format;
        int page_count = 0;
        std::string cover_url;
    };
    GalleryInfoResult register_folder(const std::string& vpath_raw, std::optional<bool> recursive);

    // gallery book → BookRef（虚拟书）
    std::optional<BookRef> book_ref(const std::string& book_id) const;

    // 图集内图片（绝对路径列表，自然排序）；供 ComicAdapter 文件夹模式用
    std::vector<std::filesystem::path> list_images(const std::filesystem::path& dir,
                                                   bool recursive) const;

private:
    const AppConfig& cfg_;
    LibraryService& lib_;
    mutable std::shared_mutex mu_;
    struct Entry {
        std::string path;      // vpath
        bool recursive = false;
        std::string title;
        int page_count = 0;
        double created_at = 0, updated_at = 0;
    };
    std::map<std::string, Entry> registry_;  // book_id → entry
};

// ------------------------------------------------------------------ layouts
struct LayoutBuildResult {
    std::string layout_id;
    std::string mode;
    int total_pages = 0;
    std::vector<nlohmann::json> chapters;  // ChapterInfo JSON
    bool cache_hit = false;
    bool strip = false;
};

class LayoutsService {
public:
    LayoutsService(const AppConfig& cfg, FontManager& fonts, LayoutsDiskCache& disk,
                   ParsedCache& parsed)
        : cfg_(cfg), fonts_(fonts), disk_(disk), parsed_(parsed),
          memory_(static_cast<size_t>(std::max(0, cfg.cache.layout_memory_max))),
          doc_memory_(static_cast<size_t>(std::max(0, cfg.cache.doc_memory_max))) {}

    // POST /layout 主逻辑（§1.3）：effective_profile → layout_key → 三级缓存
    LayoutBuildResult build(const BookRef& ref, BookAdapter& adapter,
                            const LayoutProfile& profile);
    // 内存/磁盘查找
    std::optional<LayoutEntry> entry(const std::string& book_id, const std::string& layout_id);
    // document：内存 → 按书串行化的磁盘/解析缓存（对应 Python ParsedCache.get_or_parse）
    DocumentModel document(const BookRef& ref, BookAdapter& adapter);
    // 书籍内容变化时清空该书的内存层（PageMap 与热文档）；磁盘由 context 负责
    void invalidate_book(const std::string& book_id);

    // flow 搜索定位用
    static std::optional<int> page_of_anchor(const PageMap& pm, const int target[3]);

private:
    // what 用于超时/繁忙文案：build 走"解析分页"、document 走"解析"（与 Python 一致）
    DocumentModel document_internal(const BookRef& ref, const std::string& what);

    const AppConfig& cfg_;
    FontManager& fonts_;
    LayoutsDiskCache& disk_;
    ParsedCache& parsed_;
    mutable std::shared_mutex mu_;
    // 按 key 阻塞串行化（Python KeyedLocks）：同书解析、同 (书, layout) 首次分页；
    // 等待期间不占闸门槽位
    KeyedLocks parse_locks_;
    KeyedLocks layout_locks_;
    ObjectLru<LayoutEntry> memory_{32};
    // 【最小新增】热文档内存缓存（对应 Python ParsedCache.memory / cache.doc_memory_max）：
    // Python 的内存层在 ParsedCache 内部，而 C++ 版 ParsedCache（cache/store.h 冻结）
    // 只有磁盘层，故在此补一个 DocumentModel LRU，供 document() 的 doc_memory get/put。
    ObjectLru<DocumentModel> doc_memory_{3};
};

// 关停时等待在跑的解析/分页超时任务收尾（最多 timeout_s 秒）：后台 worker 捕获了
// FontManager/AppContext 等资源，不等它们就销毁上下文会让后台线程访问悬垂对象。
// 等不到（有卡死任务）就放弃——与 Python 直接丢弃守护线程一致，只是正常关停不再冒险。
void wait_parse_tasks_idle(double timeout_s);

// ------------------------------------------------------------------ progress
class ProgressService {
public:
    explicit ProgressService(std::filesystem::path file) : file_(std::move(file)) {}

    void load();
    void flush();  // 强制落盘；内存干净时直接返回（对应 Python flush 的 dirty 检查）
    struct Record {
        std::string book_id;
        std::optional<std::string> device_id;
        // Python 的 int 无界：JSON 整数（含 2^63..2^64-1）原样存回/回显。
        // 用 nlohmann::json 保留 number_integer/number_unsigned 的原始数值类型，
        // 饱和或窄化都会与 Python 的逐字回显分叉（审查 H4）。
        nlohmann::json anchor[3] = {0, 0, 0};
        std::optional<nlohmann::json> page;
        std::optional<double> ratio;
        double updated_at_ts = 0;
        // 插入序（Python dict 语义：覆盖不改位置），仅用于按 Python 顺序落盘
        uint64_t order = 0;
        // ordered_json：字段序与 Python 一致（book_id, device_id, anchor, page, ratio, updated_at）
        nlohmann::ordered_json to_public_json() const;
    };
    // device 规范化：strip → 空→"default" → 剔除 Unicode 不可打印字符与 :"/\ → 截断 64
    static std::string normalize_device(const std::string& device);
    std::optional<Record> get(const std::string& book_id, const std::string& device) const;
    std::optional<Record> get_any(const std::string& book_id) const;
    Record put(const std::string& book_id, const std::optional<std::string>& device,
               const nlohmann::json anchor[3], std::optional<nlohmann::json> page,
               std::optional<double> ratio);

private:
    std::filesystem::path file_;
    mutable std::shared_mutex mu_;
    std::map<std::string, Record> records_;  // key = book_id:device
    // 落盘状态（Python _dirty/_last_save/_generation）：dirty 只在写盘成功后、
    // 且期间没有新写入（代数未变）时清除
    bool dirty_ = false;
    uint64_t order_seq_ = 0;
    uint64_t generation_ = 0;
    // 落盘串行化 + 节流：并发 flush 会让**旧快照后落盘**覆盖新记录，last_flush_
    // 也会变成无锁读写的 data race（审查 M6）。所有落盘都走 save_locked()。
    std::mutex flush_mu_;
    double last_flush_ = 0;
    // 调用方必须持有 flush_mu_；force=false 时按 ≥1s 节流
    bool save_locked(bool force);
    // 调用方必须持有 mu_（任意模式）
    std::string dump_locked() const;
};

// ------------------------------------------------------------------ search
struct SearchItemResult {
    int chapter_index = 0;
    std::string chapter_title;
    int block = 0, char_index = 0;
    std::optional<int> page;
    std::string snippet;
};

class SearchService {
public:
    // §4.10：纯 ASCII 查询忽略大小写；上限 500；snippet ±30
    static nlohmann::json search(const DocumentModel& doc, const std::string& query,
                                 int limit, const PageMap* pagemap);
};

}  // namespace lnv
