// MOBI / AZW3 适配器实现：ebook-convert → EPUB 缓存 → EpubAdapter
// 对应 Python formats/mobi.py；子进程用 CreateProcess + WaitForSingleObject 超时 kill。
#include "lnv/formats/mobi.h"
#include "lnv/formats/epub.h"  // 另一代理实现：需提供 create_epub_adapter(cfg, ref)
#include "lnv/cache/store.h"   // enforce_dir_quota（converted 目录 2GiB 配额）
#include "lnv/util/concurrency.h"  // KeyedLocks / heavy_gate
#include "lnv/util/paths.h"
#include "lnv/util/textutil.h"

#include <map>
#include <mutex>
#include <cstdio>

#include <windows.h>

namespace lnv {

namespace {

static constexpr double kConvertTimeoutFactor = 4.0;

// converted 目录配额：转换产物只用于复用，超限按 mtime LRU 淘汰到 80%
constexpr long long kConvertedQuotaBytes = 2LL * 1024 * 1024 * 1024;

}  // namespace

// 供 /files 的 openable 探测用（Python registry.openable_reason）。
// 与外部工具一致只查 PATH：SearchPathW 的搜索序含 exe 目录/CWD，可被种植劫持。
// 结果进程内缓存（与 rar/7z/pdfium 探测一致）：/files 的每个条目都会问一次，
// 不缓存等于每次列目录都做一遍 PATH 扫描。
bool calibre_available() {
    static const bool ok = [] { return !find_tool_in_path(L"ebook-convert").empty(); }();
    return ok;
}

namespace {

bool is_usable(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 0 && !ec;
}

void unlink_quiet(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// 每本书一把进程内锁：并发首次打开同一本书时串行转换。
// 用有界/引用计数回收的 KeyedLocks（concurrency.h）：旧的 static map<unique_ptr<mutex>>
// 会随书数无限增长（审查）。故意不析构：超时任务可能在 main 返回后仍持有锁。
KeyedLocks& convert_locks() {
    static KeyedLocks* locks = new KeyedLocks();
    return *locks;
}

// 运行子进程并限时；stderr 写入 stderr_file；返回进程 exit code
struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
};

ProcResult run_ebook_convert(const std::wstring& exe, const std::filesystem::path& input,
                             const std::filesystem::path& output,
                             const std::filesystem::path& stderr_file, double timeout_s) {
    ProcResult res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE err_h = CreateFileW(stderr_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &sa, CREATE_ALWAYS, 0, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = nul;
    si.hStdError = err_h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmdline = quote_windows_arg(exe) + L" " + quote_windows_arg(input.wstring()) +
                           L" " + quote_windows_arg(output.wstring());
    PROCESS_INFORMATION pi{};
    // CREATE_SUSPENDED：先入 Job Object 再恢复，防止子进程在入 job 前逃逸
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        res.exit_code = -1;
        return res;  // OSError 等价：启动失败
    }
    // Job Object：ebook-convert 会再拉起子进程，超时只 TerminateProcess 会留下孤儿；
    // KILL_ON_JOB_CLOSE 保证句柄关闭时兜底清理
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jinfo{};
        jinfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jinfo, sizeof(jinfo));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;  // 进程已在别的 job 中：退回只杀主进程
        }
    }
    ResumeThread(pi.hThread);
    DWORD timeout_ms = (DWORD)std::max<double>(1000.0, timeout_s * 1000.0);
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        if (job)
            TerminateJobObject(job, (UINT)-1);
        else
            TerminateProcess(pi.hProcess, (UINT)-1);  // 超时 kill 整棵进程树
        WaitForSingleObject(pi.hProcess, 5000);
        res.timed_out = true;
        res.exit_code = -1;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        res.exit_code = (int)code;
    }
    if (job) CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
}

// stderr 尾 500 字符（Python: stderr.decode("utf-8", errors="replace")[-500:]）。
// 净化统一用 textutil 的 utf8_to_u32（无效/截断序列 → U+FFFD，避免 json::dump 对
// 非法 UTF-8 抛异常，本应 422 的错误变成 500），再按码点取末 500 字符。
std::string tail_500(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return {};
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz == 0) return {};
    // 多读尾部 4KB：先净化再按字符数截取，避免按字节截断多字节序列
    size_t want = (size_t)std::min<unsigned long long>(sz, 4096);
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg((std::streamsize)(sz - want));
    std::string data(want, '\0');
    in.read(data.data(), (std::streamsize)want);
    data.resize((size_t)in.gcount());
    U32Text text = utf8_to_u32(data);
    if (text.size() > 500) text.erase(0, text.size() - 500);
    return u32_to_utf8(text);
}

}  // namespace

MobiAdapter::MobiAdapter(const AppConfig& cfg, const BookRef& ref, const AdapterEnv& env)
    : cfg_(cfg), ref_(ref), env_(env) {
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
    inner_ = create_epub_adapter(cfg_, eref, env_);
}

std::filesystem::path MobiAdapter::converted_path() const {
    auto out_dir = cfg_.cache_dir() / "converted";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    // 文件名带内容修订号（跨实现契约，与 Python 一致）：{book_id}.{rev}.epub。
    // 内容变化后新 rev 指向新文件，失效时仍在途的旧转换（先写 .tmp 再 rename）
    // 永远不会被新 rev 采用，关闭"失效与转换竞态"导致的旧 EPUB 回灌。
    if (!ref_.rev.empty()) return out_dir / (ref_.book_id + "." + ref_.rev + ".epub");
    return out_dir / (ref_.book_id + ".epub");
}

void MobiAdapter::convert(const BookRef& ref, const std::filesystem::path& target) {
    // 重活闸门：Calibre 转换可达分钟级，限流避免占满请求线程池（可重入，
    // 外层 open_adapter 已持有闸门时不会重复占槽）
    auto heavy = heavy_gate().acquire();
    auto lk = convert_locks().acquire(ref.book_id);
    if (is_usable(target)) return;  // 等锁期间别人已经转好了
    std::wstring exe = find_tool_in_path(L"ebook-convert");
    if (exe.empty())
        throw UnsupportedFormatError(
            "MOBI/AZW3 需要 Calibre 的 ebook-convert 转换，请安装 Calibre 后重试",
            "需安装 Calibre");
    double timeout = std::max(60.0, cfg_.limits.parse_timeout_s * kConvertTimeoutFactor);
    std::filesystem::path tmp = target;
    // 临时名必须仍以 .epub 结尾：ebook-convert 按输出文件的扩展名选择输出插件，
    // 传 ".tmp" 会直接失败（No plugin to handle output format: tmp）
    tmp += ".tmp.epub";  // {book_id}.epub.tmp.epub → 原子 rename
    unlink_quiet(tmp);
    std::filesystem::path err_file = tmp;
    err_file += ".err";
    auto res = run_ebook_convert(exe, ref.path, tmp, err_file, timeout);
    if (res.timed_out) {
        unlink_quiet(tmp);
        unlink_quiet(err_file);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Calibre 转换超时（>%.0fs）", timeout);
        throw ParseError(buf);
    }
    if (res.exit_code != 0 || !is_usable(tmp)) {
        std::string detail = tail_500(err_file);
        unlink_quiet(tmp);
        unlink_quiet(err_file);
        throw ParseError("Calibre 转换失败（exit=" + std::to_string(res.exit_code) + "）: " + detail);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);  // 同目录 rename = 原子 replace
    if (ec) {
        unlink_quiet(tmp);
        unlink_quiet(err_file);
        throw ParseError("转换结果无法落盘: " + ec.message());
    }
    unlink_quiet(err_file);
    // 转换产物目录配额（2GiB，按 mtime LRU 到 80%）：keep 当前文件防止刚转好被淘汰
    enforce_dir_quota(target.parent_path(), kConvertedQuotaBytes, &target);
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
