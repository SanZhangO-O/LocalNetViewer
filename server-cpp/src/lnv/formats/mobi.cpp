// MOBI / AZW3 适配器实现：ebook-convert → EPUB 缓存 → EpubAdapter
// 对应 Python formats/mobi.py；子进程用 CreateProcess + WaitForSingleObject 超时 kill。
#include "lnv/formats/mobi.h"
#include "lnv/formats/epub.h"  // 另一代理实现：需提供 create_epub_adapter(cfg, ref)

#include <map>
#include <mutex>
#include <cstdio>

#include <windows.h>

namespace lnv {

namespace {

static constexpr double kConvertTimeoutFactor = 4.0;

// 在 PATH 上定位 ebook-convert（SearchPath 语义）；找不到返回空串
std::string locate_ebook_convert() {
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, "ebook-convert", ".exe", MAX_PATH, buf, nullptr) > 0) return buf;
    return {};
}

}  // namespace

// 供 /files 的 openable 探测用（Python registry.openable_reason）
bool calibre_available() { return !locate_ebook_convert().empty(); }

namespace {

bool is_usable(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 0 && !ec;
}

void unlink_quiet(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// 每本书一把进程内锁：并发首次打开同一本书时串行转换
std::mutex& lock_for(const std::string& key) {
    static std::mutex guard;
    static std::map<std::string, std::unique_ptr<std::mutex>> locks;
    std::lock_guard<std::mutex> g(guard);
    auto it = locks.find(key);
    if (it == locks.end()) it = locks.emplace(key, std::make_unique<std::mutex>()).first;
    return *it->second;
}

// 运行子进程并限时；stderr 写入 stderr_file；返回进程 exit code
struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
};

ProcResult run_ebook_convert(const std::string& exe, const std::string& input,
                             const std::string& output, const std::filesystem::path& stderr_file,
                             double timeout_s) {
    ProcResult res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE err_h = CreateFileA(stderr_file.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &sa, CREATE_ALWAYS, 0, nullptr);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = nul;
    si.hStdError = err_h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::string cmdline = "\"" + exe + "\" \"" + input + "\" \"" + output + "\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        res.exit_code = -1;
        return res;  // OSError 等价：启动失败
    }
    DWORD timeout_ms = (DWORD)std::max<double>(1000.0, timeout_s * 1000.0);
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, (UINT)-1);  // 超时 kill
        WaitForSingleObject(pi.hProcess, 5000);
        res.timed_out = true;
        res.exit_code = -1;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        res.exit_code = (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
}

// stderr 尾 500 字节（坏字节按替换处理）
std::string tail_500(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return {};
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz == 0) return {};
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    size_t want = (size_t)std::min<unsigned long long>(sz, 500);
    in.seekg((std::streamsize)(sz - want));
    std::string data(want, '\0');
    in.read(data.data(), (std::streamsize)want);
    data.resize((size_t)in.gcount());
    // 保留可打印部分（UTF-8 尾部可能截断多字节，直接原样截尾可接受）
    return data;
}

}  // namespace

MobiAdapter::MobiAdapter(const AppConfig& cfg, const BookRef& ref)
    : cfg_(cfg), ref_(ref) {
    format = !ref.format.empty() ? ref.format : "mobi";
    mode = "flow";
    auto target = converted_path();
    if (!is_usable(target)) convert(ref_, target);  // 缓存命中则直接复用
    if (!is_usable(target)) throw ParseError("转换结果无法落盘");
    // 组装 EPUB 引用（book_id/vpath 不变，format=epub，路径指向转换产物）
    BookRef eref;
    eref.book_id = ref.book_id;
    eref.vpath = ref.vpath;
    eref.path = target;
    eref.format = "epub";
    std::error_code ec;
    eref.size = (long long)std::filesystem::file_size(target, ec);
    eref.mtime = 0.0;  // 转换产物 mtime 仅信息性，读取即可（不参与缓存键）
    eref.virtual_book = ref.virtual_book;
    eref.extra = ref.extra;
    // 依赖：formats/epub.h 需提供该工厂（另一代理实现）
    inner_ = create_epub_adapter(cfg_, eref);
}

std::filesystem::path MobiAdapter::converted_path() const {
    auto out_dir = cfg_.cache_dir() / "converted";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    return out_dir / (ref_.book_id + ".epub");
}

void MobiAdapter::convert(const BookRef& ref, const std::filesystem::path& target) {
    std::lock_guard<std::mutex> lk(lock_for(ref.book_id));
    if (is_usable(target)) return;  // 等锁期间别人已经转好了
    std::string exe = locate_ebook_convert();
    if (exe.empty())
        throw UnsupportedFormatError(
            "MOBI/AZW3 需要 Calibre 的 ebook-convert 转换，请安装 Calibre 后重试",
            "需安装 Calibre");
    double timeout = std::max(60.0, cfg_.limits.parse_timeout_s * kConvertTimeoutFactor);
    std::filesystem::path tmp = target;
    tmp += ".tmp";  // {book_id}.epub.tmp → 原子 rename
    unlink_quiet(tmp);
    std::filesystem::path err_file = tmp;
    err_file += ".err";
    auto res = run_ebook_convert(exe, ref.path.string(), tmp.string(), err_file, timeout);
    if (res.timed_out) {
        unlink_quiet(tmp);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Calibre 转换超时（>%.0fs）", timeout);
        throw ParseError(buf);
    }
    if (res.exit_code != 0 || !is_usable(tmp)) {
        std::string detail = tail_500(err_file);
        unlink_quiet(tmp);
        throw ParseError("Calibre 转换失败（exit=" + std::to_string(res.exit_code) + "）: " + detail);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);  // 同目录 rename = 原子 replace
    if (ec) {
        unlink_quiet(tmp);
        throw ParseError("转换结果无法落盘: " + ec.message());
    }
    unlink_quiet(err_file);
}

AdapterMeta MobiAdapter::meta() {
    auto m = inner_->meta();
    m.format = !ref_.format.empty() ? ref_.format : format;  // format 改回 mobi/azw3
    return m;
}

DocumentModel MobiAdapter::document() {
    auto doc = inner_->document();
    doc.format = !ref_.format.empty() ? ref_.format : format;
    return doc;
}

std::optional<ImageBytes> MobiAdapter::cover(int width) { return inner_->cover(width); }

ImageBytes MobiAdapter::asset(const std::string& path) { return inner_->asset(path); }

}  // namespace lnv
