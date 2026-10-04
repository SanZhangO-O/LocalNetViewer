// 书库服务：扫描、索引、目录列表、book_id 查找（对应 Python services/library.py）
#include "lnv/services/library.h"

#include <cmath>

#include "lnv/formats/container.h"
#include "lnv/formats/registry.h"
#include "lnv/cache/store.h"
#include "lnv/util/paths.h"
#include "lnv/util/sha1.h"

namespace lnv {
namespace {
// 宽 → UTF-8 统一走 paths.h 共享实现（不抛异常；path.string() 可能抛异常）
std::string u8_name(const std::filesystem::path& p) { return path_to_utf8(p.filename()); }
std::string u8_path(const std::filesystem::path& p) { return path_to_utf8(p); }
}  // namespace


namespace {

// ------------------------------------------------------------------ 小工具

// file_time → epoch 秒（浮点，对齐 Python st_mtime 语义）
double file_time_to_epoch(const std::filesystem::file_time_type& t) {
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::duration<double>>(sys.time_since_epoch())
        .count();
}

double path_mtime(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0.0;
    return file_time_to_epoch(t);
}

// 自然排序 helper：按数字段拆分，文本段 casefold、数字段按十进制数值比较
// （对应 Python util/natural.py 的 natural_key；空文本段跳过，与 Python 一致）。
struct NatSeg {
    bool is_num = false;
    std::string digits;  // 数字段：已去前导零，空串代表 0（Python int 语义）
    std::string text;    // casefold 后
};

void split_natural(const std::string& s, std::vector<NatSeg>& out) {
    size_t i = 0;
    std::string text_acc;
    auto flush_text = [&]() {
        if (!text_acc.empty()) {
            NatSeg seg;
            seg.text = std::move(text_acc);
            out.push_back(std::move(seg));
            text_acc.clear();
        }
    };
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= '0' && c <= '9') {
            flush_text();
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
            size_t k = i;
            while (k < j && s[k] == '0') k++;  // Python int() 忽略前导零
            NatSeg seg;
            seg.is_num = true;
            seg.digits = s.substr(k, j - k);
            out.push_back(std::move(seg));
            i = j;
        } else {
            // 文本段 casefold：ASCII 小写化（多字节 UTF-8 原样保留）
            text_acc.push_back(
                static_cast<char>((c < 128) ? static_cast<char>(std::tolower(c))
                                            : static_cast<char>(c)));
            i++;
        }
    }
    flush_text();
}

bool natural_key_less_local(const std::string& a, const std::string& b) {
    std::vector<NatSeg> ka, kb;
    split_natural(a, ka);
    split_natural(b, kb);
    size_t n = std::min(ka.size(), kb.size());
    for (size_t i = 0; i < n; i++) {
        const NatSeg& x = ka[i];
        const NatSeg& y = kb[i];
        if (x.is_num != y.is_num) return y.is_num;  // 文本段(0) 排在数字段(1) 前
        if (x.is_num) {
            // 任意精度数字的等价比法：先比有效位数，同位再字典序（无 long long 溢出）
            if (x.digits.size() != y.digits.size()) return x.digits.size() < y.digits.size();
            int c = x.digits.compare(y.digits);
            if (c != 0) return c < 0;
        } else {
            int c = x.text.compare(y.text);
            if (c != 0) return c < 0;
        }
    }
    return ka.size() < kb.size();  // 前缀相等时短者在前（与 Python 元组比较一致）
}

// 已知书籍扩展名 → 格式标识（对应 Python formats/registry.py 的 _FORMAT_BY_EXT）。
// ext 为 ext_of 输出：小写、带点。MOBI 族统一映射 "mobi"；
// 漫画族是 `_ext.lstrip(".")` —— 即 **扩展名本身**（.zip→"zip"、.rar→"rar"、
// .7z→"7z"、.tar→"tar"），不是 cbz/cbr/cb7/cbt。这个字符串会进 /files、/meta
// 和共享 index.json，写错会让两边互相认为对方条目过期并重算 book_id（审查 M6）。
const char* known_format_of(const std::string& ext) {
    static const std::map<std::string, std::string> kMap = {
        {".txt", "txt"},    {".text", "txt"}, {".log", "txt"}, {".epub", "epub"},
        {".pdf", "pdf"},    {".fb2", "fb2"},  {".mobi", "mobi"}, {".azw3", "mobi"},
        {".azw", "mobi"},   {".cbz", "cbz"},  {".zip", "zip"}, {".cbr", "cbr"},
        {".rar", "rar"},    {".cb7", "cb7"},  {".7z", "7z"},  {".cbt", "cbt"},
        {".tar", "tar"},    {".cbz3", "cbz3"},
    };
    auto it = kMap.find(ext);
    return it == kMap.end() ? nullptr : it->second.c_str();
}

// ------------------------------------------------------------------ 路径 / 计数

// 虚拟路径 → 真实路径（含穿越校验）；根名未知返回空路径
// （对应 Python LibraryService.resolve_real；library.h 冻结，作为自由函数实现）
std::filesystem::path resolve_vpath_real(const AppConfig& cfg, const std::string& vpath) {
    std::string v = normalize_vpath(vpath, true);
    std::vector<std::string> parts;
    for (auto& p : vpath_parts(v)) parts.push_back(p);
    if (parts.empty()) return {};
    const LibraryRootCfg* root = cfg.root_by_name(parts[0]);
    if (root == nullptr) return {};
    return resolve_in_root(std::filesystem::u8path(root->path),
                           std::vector<std::string>(parts.begin() + 1, parts.end()));
}

// 目录递归深度上限：链接/junction 已被跳过，这里只作兜底（伪装成普通目录的
// 异常重解析点等），防止成环把线程栈打爆。正常书库远达不到。
constexpr int kMaxScanDepth = 256;

// 目录图片数（对应 Python gallery.list_images / count_images 的计数用途）：
// 跳过隐藏与 skip_entries glob、符号链接/junction 不计。
long long count_images_in(const std::filesystem::path& dir, bool recursive,
                          const std::vector<std::string>& patterns) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    long long count = 0;
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() >= kMaxScanDepth) {  // 深度超限：只停递归，兜底防环
                it.disable_recursion_pending();
                continue;
            }
            std::error_code ec2;
            if (it->is_symlink(ec2) || ec2) { ec2.clear(); continue; }
            if (!it->is_regular_file(ec2) || ec2) {
                ec2.clear();
                // 目录若是 symlink/junction 必须阻止下钻（is_symlink 不识别 junction）
                if (it->is_directory(ec2) && !ec2 && is_symlink_or_junction(it->path()))
                    it.disable_recursion_pending();
                continue;
            }
            std::string name = u8_name(it->path());
            if (is_hidden_name(name)) continue;
            if (should_skip_entry(name, patterns)) continue;
            if (is_image_ext(ext_of(name))) count++;
        }
    } else {
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            std::error_code ec2;
            if (it->is_symlink(ec2) || ec2) { ec2.clear(); continue; }
            if (!it->is_regular_file(ec2) || ec2) { ec2.clear(); continue; }
            std::string name = u8_name(it->path());
            if (is_hidden_name(name)) continue;
            if (should_skip_entry(name, patterns)) continue;
            if (is_image_ext(ext_of(name))) count++;
        }
    }
    return count;
}

// gallery.recursive=true 时把子目录图片数累加到父目录（自底向上，对应 Python
// _rollup_gallery_counts：按路径中 "/" 数从深到浅）。模板避免命名私有嵌套类型。
template <typename MapT>
void rollup_gallery_counts(MapT& counts) {
    std::vector<std::string> ordered;
    ordered.reserve(counts.size());
    for (auto& [k, v] : counts) {
        (void)v;
        ordered.push_back(k);
    }
    std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
        size_t da = std::count(a.begin(), a.end(), '/');
        size_t db = std::count(b.begin(), b.end(), '/');
        if (da != db) return da > db;
        return a > b;  // 同深度稳定化
    });
    for (const auto& path : ordered) {
        if (path == "/") continue;
        size_t slash = path.rfind('/');
        std::string parent =
            (slash == std::string::npos || slash == 0) ? "/" : path.substr(0, slash);
        auto it = counts.find(parent);
        if (it == counts.end()) continue;
        it->second.count += counts[path].count;
    }
}

}  // namespace

// ------------------------------------------------------------------ 索引

void LibraryService::load_index() {
    auto path = cfg_.index_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(ss.str());
    } catch (...) {
        std::fprintf(stderr, "警告：索引文件损坏，已忽略: %s\n", u8_path(path).c_str());
        return;
    }
    // 字段类型错误（book_id 是数字、size 是字符串等）与 JSON 损坏同等处理：
    // 整份索引不可信 → 忽略并等下次扫描重建，绝不能让异常逃出 load_index 终止启动。
    // Python 侧损坏索引同样只记 warning 后跳过（library.py load_index）。
    std::map<std::string, LibraryBook> books;
    std::unordered_map<std::string, std::string> by_id;
    std::map<std::string, GalleryCount> counts;
    double scanned_at = 0.0;
    try {
        if (data.value("version", 0) != 1) return;  // INDEX_VERSION
        if (data.contains("books") && data["books"].is_object()) {
            for (auto& [vpath, val] : data["books"].items()) {
                if (!val.is_object() || !val.contains("book_id")) continue;
                LibraryBook b;
                b.vpath = vpath;
                b.book_id = val.value("book_id", std::string());
                b.format = val.value("format", std::string());
                b.size = val.value("size", 0LL);
                b.mtime = val.value("mtime", 0.0);
                if (b.book_id.empty()) continue;
                books[vpath] = b;
                by_id[b.book_id] = vpath;
            }
        }
        if (data.contains("gallery_counts") && data["gallery_counts"].is_object()) {
            for (auto& [vpath, val] : data["gallery_counts"].items()) {
                if (!val.is_object()) continue;
                counts[vpath] = {val.value("count", 0), val.value("mtime", 0.0)};
            }
        }
        scanned_at = data.value("scanned_at", 0.0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "警告：索引文件损坏，已忽略: %s (%s)\n",
                     u8_path(path).c_str(), e.what());
        return;
    }
    std::unique_lock lk(mu_);
    by_vpath_ = std::move(books);
    id_to_vpath_ = std::move(by_id);
    gallery_counts_ = std::move(counts);
    scanned_at_ = scanned_at;
}

void LibraryService::save_index() {
    // 串行化落盘（对齐 Python library.py 的 _save_lock）：快照 + 写盘 + 更新时间戳
    // 必须是一个临界区，否则两个并发 maybe_save 会各自写一份快照，旧快照可能后落盘。
    std::lock_guard<std::mutex> save_lk(save_mu_);
    save_index_locked();
}

void LibraryService::save_index_locked() {
    // 锁内取快照；序列化紧凑分隔符、mtime 为 epoch float（与 Python 字节级兼容）
    nlohmann::json books = nlohmann::json::object();
    nlohmann::json counts = nlohmann::json::object();
    double scanned_at;
    {
        std::unique_lock lk(mu_);
        for (auto& [vpath, b] : by_vpath_) {
            books[vpath] = {
                {"book_id", b.book_id},
                {"format", b.format},
                {"size", b.size},
                {"mtime", b.mtime},
            };
        }
        for (auto& [vpath, g] : gallery_counts_) {
            counts[vpath] = {{"count", g.count}, {"mtime", g.mtime}};
        }
        scanned_at = scanned_at_;
    }
    nlohmann::json payload = {
        {"version", 1},
        {"scanned_at", scanned_at},
        {"books", books},
        {"gallery_counts", counts},
    };
    // 写盘失败（磁盘满/权限等）必须留痕：Python save_index 失败会保留 dirty 并记
    // warnings；这里没有 dirty 标记，但扫描结束的 save_index 无条件重写，30s 节流
    // 后的 maybe_save 也会再试，不会因一次失败永久丢索引。
    if (!atomic_write(cfg_.index_path(), payload.dump())) {
        std::fprintf(stderr, "警告：索引落盘失败: %s\n",
                     u8_path(cfg_.index_path()).c_str());
    }
    last_save_ = now_epoch();
}

void LibraryService::maybe_save() {
    // Python 另有 dirty 标记；这里以 30s 节流近似（扫描结束会无条件 save_index，保证不丢）。
    // 检查与落盘都在 save_mu_ 内完成，避免两个线程同时通过检查、同时写盘。
    std::lock_guard<std::mutex> save_lk(save_mu_);
    if (now_epoch() - last_save_.load() < 30.0) return;
    save_index_locked();
}

// ------------------------------------------------------------------ 扫描

void LibraryService::scan_blocking() {
    if (scanning_.exchange(true)) return;  // 已有扫描在跑
    cancel_ = false;
    // scanning_ 必须无条件复位：书库里只要有一个 Windows 保留名目录（aux/con/nul…），
    // normalize_vpath 就会抛 PathError，而这里没有 try/finally 时扫描线程会带着
    // scanning_=true 退出 —— 之后永远 "scanning":true 且不再重扫（审查 H2）。
    struct ScanGuard {
        std::atomic<bool>* flag;
        ~ScanGuard() { flag->store(false); }
    } scan_guard{&scanning_};

    std::map<std::string, LibraryBook> books;
    std::map<std::string, GalleryCount> counts;
    const auto& patterns = cfg_.comic.skip_entries;

    // 递归遍历（os.walk 语义：目录按 natural 排序、跳过隐藏与符号链接/junction 目录/文件；
    // depth 上限兜底防成环栈溢出）
    std::function<void(const std::filesystem::path&, const std::string&,
                       const std::map<std::string, LibraryBook>&, int)>
        walk = [&](const std::filesystem::path& dirpath, const std::string& vdir,
                   const std::map<std::string, LibraryBook>& prev, int depth) {
            if (cancel_.load()) return;
            if (depth >= kMaxScanDepth) return;
            std::error_code ec;
            std::vector<std::filesystem::directory_entry> dir_entries, file_entries;
            for (std::filesystem::directory_iterator it(dirpath, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                std::error_code ec2;
                const auto& e = *it;
                std::string name = u8_name(e.path());
                if (is_hidden_name(name)) continue;
                if (e.is_symlink(ec2) || ec2) { ec2.clear(); continue; }
                if (e.is_directory(ec2) && !ec2) {
                    // is_symlink 不识别 junction；junction 成环会无限递归（Python 的
                    // os.walk 会跟进 junction，bpo-23407），这里按链接跳过
                    if (is_symlink_or_junction(e.path())) continue;
                    dir_entries.push_back(e);
                } else if (e.is_regular_file(ec2) && !ec2)
                    file_entries.push_back(e);
            }
            std::sort(dir_entries.begin(), dir_entries.end(),
                      [](const auto& a, const auto& b) {
                          return natural_key_less_local(u8_name(a.path()),
                                                  u8_name(b.path()));
                      });

            // 图片目录计数（目录自身 mtime 缓存）
            long long image_count = 0;
            for (const auto& e : file_entries) {
                if (cancel_.load()) return;
                std::string name = u8_name(e.path());
                std::string ext = ext_of(name);
                if (is_image_ext(ext)) {
                    if (!should_skip_entry(name, patterns)) image_count++;
                    continue;
                }
                const char* fmt = known_format_of(ext);
                if (!fmt) continue;
                std::string fmt_s = fmt;
                if (std::find(cfg_.library.include_ext.begin(),
                              cfg_.library.include_ext.end(),
                              ext) == cfg_.library.include_ext.end() &&
                    !cfg_.library.show_all_files)
                    continue;
                long long size = static_cast<long long>(e.file_size(ec));
                if (ec) { ec.clear(); continue; }
                double mtime = path_mtime(e.path());
                std::string vpath;
                try { vpath = join_vpath(vdir, {name}); }
                catch (const std::exception&) {
                    throw;
                }
                // 增量：size 相同且 |mtime 差| < 1e-6 且 format 相同 → 复用 book_id
                std::string book_id;
                auto pit = prev.find(vpath);
                if (pit != prev.end() && pit->second.size == size &&
                    std::fabs(pit->second.mtime - mtime) < 1e-6 &&
                    pit->second.format == fmt_s) {
                    book_id = pit->second.book_id;
                } else {
                    book_id = compute_book_id(e.path(), size);
                }
                LibraryBook b;
                b.book_id = book_id;
                b.vpath = vpath;
                b.format = fmt_s;
                b.size = size;
                b.mtime = mtime;
                books[vpath] = std::move(b);
            }
            counts[vdir] = {static_cast<int>(image_count), path_mtime(dirpath)};

            for (const auto& d : dir_entries) {
                if (cancel_.load()) return;
                walk(d.path(), join_vpath(vdir, {u8_name(d.path())}), prev, depth + 1);
            }
        };

    std::map<std::string, LibraryBook> prev;
    {
        std::shared_lock lk(mu_);
        prev = by_vpath_;
    }

    for (const auto& root : cfg_.library.roots) {
        if (cancel_.load()) break;
        std::filesystem::path root_path = std::filesystem::u8path(root.path);
        if (!std::filesystem::is_directory(root_path)) continue;  // 根不存在，跳过
        walk(root_path, "/" + root.name, prev, 0);
    }
    if (cfg_.gallery.recursive) {
        // recursive=true：目录计数自底向上 rollup（父 = 子孙之和）
        rollup_gallery_counts(counts);
    }

    std::map<std::string, LibraryBook> live_before;
    {
        std::unique_lock lk(mu_);
        // 扫描期间请求线程可能已检测到变化、刷新 live 索引并通知过；
        // 提交前快照，供下面的通知循环去重（否则同一次变化会清两遍缓存）。
        live_before = by_vpath_;
        by_vpath_.clear();
        id_to_vpath_.clear();
        for (auto& [vpath, b] : books) {
            LibraryBook nb = b;
            nb.vpath = vpath;
            by_vpath_[vpath] = std::move(nb);
            id_to_vpath_[b.book_id] = vpath;
        }
        gallery_counts_ = counts;
        scanned_at_ = now_epoch();
    }
    // 扫描路径的失效语义（对应 Python 在 scan 路径的修复）：本地文件 size/mtime/
    // format 变化或被删除 → 通知清旧 book_id 的派生缓存。否则扫描只刷新索引，
    // 旧分页/旧页面图会一直服务到进程重启（"一个路径静默刷新 size/mtime，
    // 后一个路径再也检测不到变化"）。通知在 mu_ 外做，避免与缓存锁嵌套。
    for (const auto& [vpath, pb] : prev) {
        if (pb.book_id.empty()) continue;
        auto it = books.find(vpath);
        bool changed = it == books.end();
        if (!changed) {
            const LibraryBook& nb = it->second;
            changed = nb.book_id != pb.book_id || nb.size != pb.size ||
                      std::fabs(nb.mtime - pb.mtime) > 1e-6 || nb.format != pb.format;
        }
        if (!changed) continue;
        // 请求线程可能在扫描期间已抢先处理同一变化：live 已是本次扫描的新值、
        // 且不同于扫描起点的 prev → 跳过，避免二次失效清掉刚重建的缓存
        // （Python `_claim_scan_change` 的等价去重）。
        if (it != books.end()) {
            auto lit = live_before.find(vpath);
            if (lit != live_before.end()) {
                const LibraryBook& nb = it->second;
                const LibraryBook& lb = lit->second;
                const bool live_is_new =
                    lb.book_id == nb.book_id && lb.size == nb.size &&
                    std::fabs(lb.mtime - nb.mtime) < 1e-6 && lb.format == nb.format;
                const bool live_is_same_as_prev =
                    lb.book_id == pb.book_id && lb.size == pb.size &&
                    std::fabs(lb.mtime - pb.mtime) < 1e-6 && lb.format == pb.format;
                if (live_is_new && !live_is_same_as_prev) continue;
            }
        }
        notify_content_changed(pb.book_id);
    }
    save_index();  // Python 在扫描锁释放后落盘（scanning_ 由 ScanGuard 复位）
}

void LibraryService::start_background_scan() {
    if (scan_thread_.joinable()) scan_thread_.join();
    if (scanning_.load()) return;
    // 线程体内必须吞掉所有异常：逃逸到 std::thread 入口会直接 std::terminate
    // 整个进程（审查 H2）。失败只记录，不影响服务继续运行。
    scan_thread_ = std::thread([this]() {
        try {
            scan_blocking();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "警告：后台扫描失败：%s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "警告：后台扫描失败（未知异常）\n");
        }
    });
}

void LibraryService::cancel_scan() { cancel_ = true; }

// 协作式等待：扫描循环每层都检查 cancel_，因此置位后很快退出；这里再 join，
// 保证 shutdown 之后不存在 joinable 的后台线程（否则进程会 abort，审查 H1）
void LibraryService::wait_idle(double timeout_s) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(timeout_s * 1000));
    while (scanning_.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (scan_thread_.joinable()) scan_thread_.join();
}

bool LibraryService::scanning() const { return scanning_.load(); }

// ------------------------------------------------------------------ 统计

std::vector<LibraryService::LibCount> LibraryService::libraries_info() const {
    std::vector<LibCount> out;
    std::map<std::string, LibraryBook> snapshot;
    {
        std::shared_lock lk(mu_);
        snapshot = by_vpath_;
    }
    for (const auto& root : cfg_.library.roots) {
        std::string prefix = "/" + root.name;
        int count = 0;
        for (auto& [vp, b] : snapshot) {
            (void)b;
            if (vp == prefix || vp.rfind(prefix + "/", 0) == 0) count++;
        }
        out.push_back({root.name, count, root.path});  // path 为真实路径字符串
    }
    return out;
}

// ------------------------------------------------------------------ 目录列表

nlohmann::json LibraryService::list_dir(const std::string& vpath_raw, int limit,
                                        long long offset) const {
    // list_dir 名义上是 const，但 Python 版会因 book_id 重算而更新索引
    // （_book_id_for 副作用）；对象本身并非 const，这里用 const_cast 保持头文件签名。
    auto* self = const_cast<LibraryService*>(this);

    std::string v;
    try {
        v = normalize_vpath(vpath_raw, true);
    } catch (const PathError& e) {
        throw AppError(ErrorCode::BadPath, e.what());
    }

    if (v == "/") {
        // ---- 书库根列表 ----
        std::vector<nlohmann::json> entries;
        for (const auto& root : cfg_.library.roots) {
            std::filesystem::path p = std::filesystem::u8path(root.path);
            bool exists = std::filesystem::is_directory(p);
            entries.push_back({
                {"name", root.name},
                {"type", "dir"},
                {"path", "/" + root.name},
                {"format", nullptr},
                {"book_id", nullptr},
                {"size", nullptr},
                {"mtime", nullptr},
                {"cover_url", nullptr},
                {"gallery", nullptr},
                {"openable", exists},
                {"reason", exists ? nlohmann::json(nullptr)
                                  : nlohmann::json("书库根目录不存在")},
            });
        }
        int total = static_cast<int>(entries.size());
        long long off = std::max<long long>(0, offset);
        int lim = std::max(1, limit);
        size_t start = off > (long long)entries.size() ? entries.size() : (size_t)off;
        std::vector<nlohmann::json> page(
            entries.begin() + start,
            entries.begin() + std::min<size_t>(start + (size_t)lim, entries.size()));
        nlohmann::json libs = nlohmann::json::array();
        for (auto& lc : libraries_info()) {
            libs.push_back(
                {{"name", lc.name}, {"book_count", lc.book_count}, {"path", *lc.path}});
        }
        return {
            {"path", "/"},
            {"parent", nullptr},
            {"libraries", libs},
            {"gallery", nullptr},
            {"entries", page},
            {"total", total},
            {"limit", lim},
            {"offset", off},
            {"scanning", scanning()},
        };
    }

    // ---- 子目录列表 ----
    std::vector<std::string> parts;
    for (auto& p : vpath_parts(v)) parts.push_back(p);
    if (parts.empty()) throw AppError(ErrorCode::BadPath, "书库不存在: " + v);
    const LibraryRootCfg* root = cfg_.root_by_name(parts[0]);
    if (root == nullptr) throw AppError(ErrorCode::BadPath, "书库不存在: " + parts[0]);

    std::filesystem::path real;
    try {
        real = resolve_in_root(std::filesystem::u8path(root->path),
                               std::vector<std::string>(parts.begin() + 1, parts.end()));
    } catch (const PathError& e) {
        throw AppError(ErrorCode::BadPath, e.what());
    } catch (const TraversalError& e) {
        throw AppError(ErrorCode::PathTraversal, e.what());
    }

    std::error_code ec;
    if (!std::filesystem::exists(real, ec))
        throw AppError(ErrorCode::BadPath, "路径不存在: " + v);
    if (!std::filesystem::is_directory(real, ec))
        throw AppError(ErrorCode::BadPath, "不是目录: " + v);

    // 目录图片数查询（命中索引缓存则不重新遍历；recursive 模式缓存存整棵子树，
    // 子目录变化不改父 mtime → 不使用缓存，保证计数正确）
    auto dir_gallery_info = [&](const std::string& cvp,
                                const std::filesystem::path& r) -> int {
        double mtime = path_mtime(r);
        if (!cfg_.gallery.recursive) {
            std::shared_lock lk(mu_);
            auto it = gallery_counts_.find(cvp);
            if (it != gallery_counts_.end() && std::fabs(it->second.mtime - mtime) < 1e-6)
                return it->second.count;
        }
        long long count = count_images_in(r, cfg_.gallery.recursive, cfg_.comic.skip_entries);
        std::unique_lock lk(mu_);
        self->gallery_counts_[cvp] = {static_cast<int>(count), mtime};
        return static_cast<int>(count);
    };

    std::vector<std::filesystem::directory_entry> entries_raw;
    try {
        std::error_code iec;
        std::filesystem::directory_iterator it(real, iec), end;
        if (iec) throw std::runtime_error("open");
        for (; it != end; it.increment(iec)) {
            if (iec) throw std::runtime_error("read");
            entries_raw.push_back(*it);
        }
    } catch (const std::exception&) {
        throw AppError(ErrorCode::BadPath, "目录无法读取: " + v);
    }

    const auto& patterns = cfg_.comic.skip_entries;
    int min_images = std::max(1, cfg_.gallery.min_images_in_folder);
    std::vector<nlohmann::json> dirs, files;

    for (const auto& e : entries_raw) {
        std::error_code ec2;
        std::string name = u8_name(e.path());
        if (is_hidden_name(name)) continue;
        if (e.is_symlink(ec2) || ec2) { ec2.clear(); continue; }
        bool is_dir = e.is_directory(ec2);
        if (ec2) { ec2.clear(); continue; }
        long long size = static_cast<long long>(e.file_size(ec2));
        if (ec2) size = 0;
        double mtime = path_mtime(e.path());
        std::string child_vpath = join_vpath(v, {name});

        if (is_dir) {
            // 目录条目：type "dir" + gallery 桶；名字无法表示成虚拟路径时置灰
            int img_count = dir_gallery_info(child_vpath, e.path());
            bool navigable = true;
            std::string reason;
            try {
                normalize_vpath(child_vpath);
            } catch (const PathError& exc) {
                navigable = false;
                reason = std::string("目录名服务器不支持：") + exc.what();
            }
            bool can_open = cfg_.gallery.folder_as_book && img_count >= min_images && navigable;
            dirs.push_back({
                {"name", name},
                {"type", "dir"},
                {"path", child_vpath},
                {"format", nullptr},
                {"book_id", nullptr},
                {"size", nullptr},
                {"mtime", nullptr},
                {"cover_url", nullptr},
                {"gallery",
                 {{"image_count", img_count}, {"can_open_as_book", can_open}}},
                {"openable", navigable},
                {"reason", reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(reason)},
            });
            continue;
        }

        std::string ext = ext_of(name);
        if (is_image_ext(ext) && cfg_.gallery.show_image_files) {
            if (should_skip_entry(name, patterns)) continue;
            // 单张图片按"单页图集"打开，但注册要求图片数 ≥ min_images_in_folder，
            // 所以只有该阈值为 1 时才能真的打开（列表不能谎报可打开）
            bool can_open = cfg_.gallery.folder_as_book && min_images <= 1;
            std::string reason;
            if (!cfg_.gallery.folder_as_book)
                reason = "图集功能已关闭";
            else if (!can_open)
                reason = "单张图片需要 gallery.min_images_in_folder ≤ 1（当前 " +
                         std::to_string(min_images) + "）";
            // Python library.py:499 用 gallery_book_id(child_vpath)，而它哈希的是
            // **规范化后**的虚拟路径（services.cpp register_folder 也走同一条规范化）。
            // 直接用 join_vpath 的原始结果，遇到含 %xx 或首尾空格的名字会得到
            // 与注册表不一致的 book_id，客户端拿着它请求 /meta 只会 404。
            std::string norm_vpath;
            try {
                norm_vpath = normalize_vpath(child_vpath, /*allow_empty=*/true);
            } catch (...) {
                norm_vpath = child_vpath;
            }
            std::string book_id = stable_hash("gallery:" + norm_vpath, 16);
            files.push_back({
                {"name", name},
                {"type", "image"},
                {"path", child_vpath},
                {"format", "gallery"},
                {"book_id", book_id},
                {"size", size},
                {"mtime", iso_time_utc_micro(mtime)},
                {"cover_url", nullptr},
                {"gallery", {{"image_count", 1}, {"can_open_as_book", can_open}}},
                {"openable", can_open},
                {"reason", reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(reason)},
            });
            continue;
        }

        const char* fmt = known_format_of(ext);
        if (!fmt) {
            if (cfg_.library.show_all_files) {
                files.push_back({
                    {"name", name},
                    {"type", "file"},
                    {"path", child_vpath},
                    {"format", nullptr},
                    {"book_id", nullptr},
                    {"size", size},
                    {"mtime", iso_time_utc_micro(mtime)},
                    {"cover_url", nullptr},
                    {"gallery", nullptr},
                    {"openable", false},
                    {"reason", "非电子书格式"},
                });
            }
            continue;
        }
        if (std::find(cfg_.library.include_ext.begin(), cfg_.library.include_ext.end(), ext) ==
                cfg_.library.include_ext.end() &&
            !cfg_.library.show_all_files)
            continue;

        // book_id：索引增量命中则复用，否则重算指纹并更新索引映射
        std::string fmt_s = fmt;
        std::string book_id;
        std::string changed_old_id;  // 非空 → 内容变化，锁外通知清派生缓存
        {
            bool reuse = false;
            std::shared_lock lk(mu_);
            auto it = by_vpath_.find(child_vpath);
            // 与 Python _book_id_for 对齐：还要求旧记录有 book_id（损坏索引不复用空 id）
            if (it != by_vpath_.end() && it->second.size == size &&
                std::fabs(it->second.mtime - mtime) < 1e-6 && it->second.format == fmt_s &&
                !it->second.book_id.empty()) {
                book_id = it->second.book_id;
                reuse = true;
            }
            if (!reuse) {
                lk.unlock();
                std::string computed = compute_book_id(e.path(), size);
                std::unique_lock lk2(mu_);
                // 并发重复检测（对齐 Python _book_id_for）：另一个线程已完成切换并
                // 通知过旧 id，直接沿用其结果，避免同一次变化通知两次。
                auto bit = self->by_vpath_.find(child_vpath);
                if (bit != self->by_vpath_.end() && bit->second.size == size &&
                    std::fabs(bit->second.mtime - mtime) < 1e-6 &&
                    bit->second.format == fmt_s && !bit->second.book_id.empty()) {
                    book_id = bit->second.book_id;
                } else {
                    book_id = computed;
                    // 同一 vpath 换了内容 → 旧 book_id 必须彻底作废，否则
                    // id_to_vpath_ 里会留下"指向同一路径、却声称自己是旧 id"的别名。
                    // 同时记录旧 id 供锁外通知：列表路径不能让 size/mtime 被静默
                    // 刷新后旧分页/旧页面图继续服务（审查：三路失效不一致）。
                    if (bit != self->by_vpath_.end()) changed_old_id = bit->second.book_id;
                    LibraryBook nb;
                    nb.book_id = book_id;
                    nb.vpath = child_vpath;
                    nb.format = fmt_s;
                    nb.size = size;
                    nb.mtime = mtime;
                    self->by_vpath_[child_vpath] = std::move(nb);
                    if (!changed_old_id.empty() && changed_old_id != book_id) {
                        auto pit = self->id_to_vpath_.find(changed_old_id);
                        if (pit != self->id_to_vpath_.end() && pit->second == child_vpath)
                            self->id_to_vpath_.erase(pit);
                    }
                    self->id_to_vpath_[book_id] = child_vpath;
                }
            }
        }
        // 等长就地编辑时旧 id 可能等于新 id，但派生缓存（分页/解析/页面图）仍必须清
        if (!changed_old_id.empty()) notify_content_changed(changed_old_id);
        const std::string rev = compute_file_rev(book_id, size, mtime);
        // openable/reason：后端缺失（unrar/7z/Calibre/pdfium）时置灰并给出原因。
        // 对应 Python library.py:536-549 的 registry.openable_reason(fmt)：
        // 恒 true 会让客户端把打不开的书显示成可点，点进去才报 415。
        std::optional<std::string> unopenable = openable_reason(fmt_s);
        files.push_back({
            {"name", name},
            {"type", "book"},
            {"path", child_vpath},
            {"format", fmt_s},
            {"book_id", book_id},
            {"size", size},
            {"mtime", iso_time_utc_micro(mtime)},
            {"cover_url", "/api/v1/books/" + book_id + "/cover?rev=" + rev},
            {"gallery", nullptr},
            {"openable", !unopenable.has_value()},
            {"reason", unopenable ? nlohmann::json(*unopenable) : nlohmann::json(nullptr)},
        });
    }

    std::sort(dirs.begin(), dirs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return natural_key_less_local(a["name"].get<std::string>(), b["name"].get<std::string>());
    });
    std::sort(files.begin(), files.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return natural_key_less_local(a["name"].get<std::string>(), b["name"].get<std::string>());
    });
    std::vector<nlohmann::json> entries = std::move(dirs);
    entries.insert(entries.end(), std::make_move_iterator(files.begin()),
                   std::make_move_iterator(files.end()));

    int total = static_cast<int>(entries.size());
    long long off = std::max<long long>(0, offset);
    int lim = std::max(1, limit);
    size_t start = off > (long long)entries.size() ? entries.size() : (size_t)off;
    std::vector<nlohmann::json> page(
        entries.begin() + start,
        entries.begin() + std::min<size_t>(start + (size_t)lim, entries.size()));

    int gcount = dir_gallery_info(v, real);
    bool can_open_root = cfg_.gallery.folder_as_book && gcount >= min_images;
    auto parent = parent_vpath(v);

    return {
        {"path", v},
        {"parent", parent ? nlohmann::json(*parent) : nlohmann::json(nullptr)},
        {"libraries", nullptr},
        {"gallery", {{"image_count", gcount}, {"can_open_as_book", can_open_root}}},
        {"entries", page},
        {"total", total},
        {"limit", lim},
        {"offset", off},
        {"scanning", scanning()},
    };
}

// ------------------------------------------------------------------ book_id / 引用

std::optional<BookRef> LibraryService::require_book(const std::string& book_id) const {
    std::string bid = book_id;
    size_t b = bid.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    size_t e = bid.find_last_not_of(" \t\r\n");
    bid = bid.substr(b, e - b + 1);

    std::string vpath;
    LibraryBook info;
    {
        std::shared_lock lk(mu_);
        auto it = id_to_vpath_.find(bid);
        if (it == id_to_vpath_.end()) return std::nullopt;
        vpath = it->second;
        auto bit = by_vpath_.find(vpath);
        if (bit != by_vpath_.end()) info = bit->second;
    }
    std::filesystem::path real;
    try {
        real = resolve_vpath_real(cfg_, vpath);
    } catch (...) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(real, ec)) return std::nullopt;
    // 索引里记录的 book_id 已不是请求的这个 → 这是被替换掉的旧指纹，
    // 不能按 vpath 继续提供文件（否则等于用旧 id 交付新内容）
    if (!info.book_id.empty() && info.book_id != bid) return std::nullopt;
    if (!info.book_id.empty()) {
        // 就地替换检测：book_id 是内容指纹，尺寸/时间变了必须重算
        long long size = static_cast<long long>(std::filesystem::file_size(real, ec));
        if (ec) return std::nullopt;
        double mtime = path_mtime(real);
        if (info.size != size || std::fabs(info.mtime - mtime) > 1e-6) {
            std::string refreshed = compute_book_id(real, size);
            // 索引刷新（对应 Python book_by_id → _book_id_for）：写入新指纹并作废
            // 旧 id 别名。只刷新 size/mtime 而不更新映射的话，下一次请求会把旧 id
            // 继续当作有效指纹（"一个路径悄悄刷新、后一个路径再也检测不到变化"）。
            {
                auto& mutable_books =
                    const_cast<std::map<std::string, LibraryBook>&>(by_vpath_);
                auto& mutable_ids = const_cast<std::unordered_map<std::string, std::string>&>(
                    id_to_vpath_);
                std::unique_lock lk(mu_);
                auto bit = mutable_books.find(vpath);
                if (bit != mutable_books.end()) {
                    std::string previous = bit->second.book_id;
                    bit->second.book_id = refreshed;
                    bit->second.size = size;
                    bit->second.mtime = mtime;
                    if (!previous.empty() && previous != refreshed) {
                        auto pit = mutable_ids.find(previous);
                        if (pit != mutable_ids.end() && pit->second == vpath)
                            mutable_ids.erase(pit);
                    }
                    mutable_ids[refreshed] = vpath;
                }
            }
            // 内容变了：清掉旧 book_id 的全部派生缓存（对应 library.py:668
            // _notify_content_changed）。等长就地编辑时 book_id 不变，
            // 不通知就会一直用旧的 parsed/layout 缓存与旧页面图。
            notify_content_changed(bid);
            if (refreshed != bid) return std::nullopt;  // 内容指纹真的变了：旧 id 失效
            info.size = size;
            info.mtime = mtime;
        }
    } else {
        // 防御（对齐 Python book_by_id）：_by_id 有记录但索引无详情时补一次 stat，
        // 否则 rev 会按 size=0/mtime=0 计算，与 Python 分叉
        std::error_code sec;
        long long size = static_cast<long long>(std::filesystem::file_size(real, sec));
        if (!sec) {
            info.size = size;
            info.mtime = path_mtime(real);
        }
    }
    BookRef ref;
    ref.book_id = bid;
    ref.rev = compute_file_rev(bid, info.size, info.mtime);
    ref.vpath = vpath;
    ref.path = real;
    ref.format = info.format;
    ref.size = info.size;
    ref.mtime = info.mtime;
    ref.virtual_book = false;
    return ref;
}

void LibraryService::add_content_listener(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(listener_mu_);
    listeners_.push_back(std::move(cb));
}

void LibraryService::notify_content_changed(const std::string& book_id) const {
    std::vector<std::function<void(const std::string&)>> copy;
    {
        std::lock_guard<std::mutex> lk(listener_mu_);
        copy = listeners_;
    }
    // 锁外调用：监听器会去拿布局/页面缓存的锁，不能与 listener_mu_ 形成嵌套
    for (auto& cb : copy) {
        try {
            cb(book_id);
        } catch (...) {
            // 清缓存失败不应影响请求
        }
    }
}

std::optional<BookRef> LibraryService::refresh_book(const std::string& book_id) {
    // 与 require_book 相同的指纹校验；区别在于文件变化时总是重算指纹并更新
    // 索引映射，返回**当前 id** 的引用（id 变了即新 id；派生缓存由监听器清理）
    std::string vpath;
    LibraryBook info;
    {
        std::shared_lock lk(mu_);
        auto it = id_to_vpath_.find(book_id);
        if (it == id_to_vpath_.end()) return std::nullopt;
        vpath = it->second;
        auto bit = by_vpath_.find(vpath);
        if (bit != by_vpath_.end()) info = bit->second;
    }
    std::filesystem::path real;
    try {
        real = resolve_vpath_real(cfg_, vpath);
    } catch (...) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(real, ec)) return std::nullopt;
    long long size = static_cast<long long>(std::filesystem::file_size(real, ec));
    if (ec) return std::nullopt;
    double mtime = path_mtime(real);
    bool changed = info.size != size || std::fabs(info.mtime - mtime) > 1e-6;
    if (changed) {
        std::string new_id = compute_book_id(real, size);
        std::unique_lock lk(mu_);
        std::string previous = info.book_id;
        info.book_id = new_id;
        info.size = size;
        info.mtime = mtime;
        by_vpath_[vpath] = info;
        if (!previous.empty() && previous != new_id) {
            auto pit = id_to_vpath_.find(previous);
            if (pit != id_to_vpath_.end() && pit->second == vpath) id_to_vpath_.erase(pit);
        }
        id_to_vpath_[new_id] = vpath;
        lk.unlock();
        // 内容变了 → 丢弃旧 book_id 的全部派生缓存（布局/解析/页面图/封面）。
        // Python 在 library.py:668 调 _notify_content_changed(old_id)。
        if (!previous.empty()) notify_content_changed(previous);
    }
    BookRef ref;
    ref.book_id = info.book_id;
    ref.rev = compute_file_rev(info.book_id, info.size, info.mtime);
    ref.vpath = vpath;
    ref.path = real;
    ref.format = info.format;
    ref.size = info.size;
    ref.mtime = info.mtime;
    ref.virtual_book = false;
    return ref;
}

std::vector<LibraryRootInfo> LibraryService::roots() const {
    std::vector<LibraryRootInfo> out;
    for (const auto& r : cfg_.library.roots)
        out.push_back({r.name, std::filesystem::u8path(r.path)});
    return out;
}

}  // namespace lnv
