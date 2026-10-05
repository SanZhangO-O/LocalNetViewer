#pragma once
// 书库服务：扫描、索引、目录列表、book_id 查找（对应 Python services/library.py）
#include <shared_mutex>
#include "lnv/auth.h"  // AccessScope（按密码访问不同书库，设计 9）
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

struct LibraryBook {
    std::string book_id;
    std::string vpath;
    std::string format;
    long long size = 0;
    double mtime = 0.0;
};

struct LibraryRootInfo {
    std::string name;
    std::filesystem::path path;
};

class LibraryService {
public:
    explicit LibraryService(const AppConfig& cfg) : cfg_(cfg) {}
    // 兜底：析构时绝不允许留下 joinable 线程（~std::thread 会 std::terminate）
    ~LibraryService() {
        if (scan_thread_.joinable()) {
            cancel_ = true;
            scan_thread_.join();
        }
    }

    // 读取 cache/index.json；不存在则空索引
    void load_index();
    // 同步全量扫描（增量：size+mtime+format 未变则复用 book_id）
    void scan_blocking();
    // 后台扫描控制
    void start_background_scan();
    void cancel_scan();
    // 协作式等待后台扫描结束并 join（对应 Python library.wait_idle(10.0)）：
    // 缺了它，main 返回时 ~std::thread 会析构一个 joinable 线程 → std::terminate（审查 H1）
    void wait_idle(double timeout_s);
    bool scanning() const;
    // 索引节流落盘（最短间隔 30s）
    void maybe_save();
    void save_index();

    // /server/info 与 /files 的 libraries 列表（book_count 从索引统计）
    // allowed 非空时只返回访问范围内的书库（nullptr = 全部）
    struct LibCount { std::string name; int book_count; std::optional<std::string> path; };
    std::vector<LibCount> libraries_info(const AccessScope* allowed = nullptr) const;

    // 虚拟路径 → 目录列表（§1.3 FilesResponse 的 entries 数据部分）。
    // 输出的 entry JSON 字段与 Python 一致（name/type/path/format/book_id/size/mtime/cover_url/gallery/openable/reason）。
    // libraries 字段仅 path=="/" 时由调用方补充。
    // offset 用 long long：Python 的查询参数是无界 int，响应里要**原样回显**，
    // 且巨大 offset 必须得到空列表而不是被窄化成 0（审查 H4）
    // allowed 非空时只列访问范围内的书库根；范围外路径抛 BadPath（与"不存在"一致，防探测）
    nlohmann::json list_dir(const std::string& vpath_raw, int limit, long long offset,
                            const AccessScope* allowed = nullptr) const;

    // 虚拟路径首段书库名（无 / 非法 → 空串）
    static std::string root_name_of(const std::string& vpath);
    // 书库不在访问范围内 → 抛 BadPath（与"书库不存在"不可区分）
    void ensure_path_allowed(const std::string& vpath, const AccessScope* allowed) const;

    // book_id 查找（含图集注册表里的虚拟书）；不存在 → nullopt
    std::optional<BookRef> require_book(const std::string& book_id) const;

    // 内容变化监听（对应 Python library.py::add_content_listener）：文件的
    // size/mtime 变化时通知上层丢弃该 book_id 的全部派生缓存（布局/解析/页面图/封面）。
    // 少了这一步，"等长就地编辑"（book_id 不变）会让服务器永远返回旧分页与旧页面图。
    void add_content_listener(std::function<void(const std::string&)> cb);
    // const：require_book（const）里检测到指纹变化时也要通知
    void notify_content_changed(const std::string& book_id) const;

    // 书籍指纹校验：size/mtime 与索引不符时重算指纹；变化时清缓存并更新索引
    // 返回 null 表示书不存在
    std::optional<BookRef> refresh_book(const std::string& book_id);

    std::vector<LibraryRootInfo> roots() const;
    const AppConfig& config() const { return cfg_; }

    // 编码覆盖等代理
    std::optional<std::string> encoding_override(const std::string& vpath) const {
        return cfg_.encoding_override(vpath);
    }

private:
    // 落盘主体（调用方必须已持有 save_mu_）
    void save_index_locked();

    const AppConfig& cfg_;
    mutable std::shared_mutex mu_;
    // vpath → book
    std::map<std::string, LibraryBook> by_vpath_;
    std::unordered_map<std::string, std::string> id_to_vpath_;
    // gallery_counts: vpath → {count, mtime}
    struct GalleryCount { int count; double mtime; };
    std::map<std::string, GalleryCount> gallery_counts_;
    double scanned_at_ = 0;
    // 落盘节流时间戳 + 落盘互斥：12 线程池里每个 /files 都可能触发 maybe_save，
    // 两个线程同时通过节流检查会并发写同一个 index.json（Python 用 _save_lock 串行化）
    std::atomic<double> last_save_{0};
    std::mutex save_mu_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::thread scan_thread_;
    mutable std::mutex listener_mu_;
    mutable std::vector<std::function<void(const std::string&)>> listeners_;
};

}  // namespace lnv
