// 容器实现（对应 Python formats/comics.py 的容器层 + zipfile/tarfile 语义）
// ZIP 用 miniz；TAR 自写 ustar/pax 读取；RAR/7z 借外部工具：列表后用
// unrar p / 7z e -so 按需把单条目解到内存（不整包落盘，见 ExternalContainer）。
#include "lnv/formats/container.h"
#include "lnv/util/concurrency.h"  // heavy_gate（外部子进程解压/列表）
#include "lnv/util/paths.h"
#include "lnv/util/textutil.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <fstream>
#include <cstring>
#include <limits>

#include <windows.h>
#include <miniz.h>

namespace lnv {

// 解压总量超限的错误文案（container/epub/comics 共用；逐字对齐 Python）
std::string archive_total_limit_error(long long total_bytes) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "压缩包解压总量超过上限（%.1fMB），已拒绝",
                  total_bytes / 1048576.0);
    return buf;
}

// ============================================================ 小工具

// posixpath.normpath：折叠 // 与 . 段，处理 .. 语义；空结果为 "."
static std::string posix_normpath(const std::string& p) {
    std::string s = p;
    bool abs = !s.empty() && s[0] == '/';
    std::vector<std::string> st;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string seg = s.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!st.empty() && st.back() != "..") st.pop_back();
            else if (!abs) st.push_back("..");
            continue;
        }
        st.push_back(seg);
    }
    std::string out;
    for (auto& seg : st) { out += seg; out += '/'; }
    if (!out.empty()) out.pop_back();
    if (abs) out = "/" + out;
    return out.empty() ? "." : out;
}

// ---------------------------------------------------------------- fnmatch
// 照抄 Python fnmatch 语义：* 跨任意字符（含 /），? 单字符，[seq] 字符类，
// Windows 上 normcase 使匹配大小写不敏感。这里写经典回溯 glob，按字节比较、
// ASCII 字母忽略大小写（UTF-8 高位字节不受影响）。
static int pat_class_match(const std::string& pat, size_t& i, unsigned char c) {
    // pat[i] == '['；返回是否命中，并把 i 推进到 ']' 之后
    size_t j = i + 1;
    bool neg = false;
    if (j < pat.size() && (pat[j] == '!' || pat[j] == '^')) { neg = true; ++j; }
    bool matched = false;
    bool first = true;
    while (j < pat.size() && (pat[j] != ']' || first)) {
        first = false;
        unsigned char lo = (unsigned char)pat[j];
        if (j + 2 < pat.size() && pat[j + 1] == '-' && pat[j + 2] != ']') {
            unsigned char hi = (unsigned char)pat[j + 2];
            unsigned char lc = (unsigned char)std::tolower(c);
            if (lc >= (unsigned char)std::tolower(lo) && lc <= (unsigned char)std::tolower(hi)) matched = true;
            j += 3;
        } else {
            unsigned char lc = (unsigned char)std::tolower(c);
            if (lc == (unsigned char)std::tolower(lo)) matched = true;
            ++j;
        }
    }
    if (j >= pat.size()) return -1;  // 未闭合 → 整个模式按字面处理
    i = j + 1;
    return matched != neg ? 1 : 0;
}

static bool glob_match_impl(const std::string& name, size_t ni,
                            const std::string& pat, size_t pi) {
    while (pi < pat.size()) {
        char pc = pat[pi];
        if (pc == '*') {
            // 连续 * 合并
            while (pi < pat.size() && pat[pi] == '*') ++pi;
            if (pi >= pat.size()) return true;
            for (size_t k = ni; k <= name.size(); ++k)
                if (glob_match_impl(name, k, pat, pi)) return true;
            return false;
        }
        if (ni >= name.size()) return false;
        unsigned char c = (unsigned char)name[ni];
        if (pc == '?') { ++ni; ++pi; continue; }
        if (pc == '[') {
            int r = pat_class_match(pat, pi, c);
            if (r < 0) {  // 未闭合 '['：Python 行为是按字面匹配 '['
                if (c != '[') return false;
                ++ni; ++pi; continue;
            }
            if (r != 1) return false;
            ++ni; continue;
        }
        unsigned char lc = (unsigned char)std::tolower(c);
        if (lc != (unsigned char)std::tolower((unsigned char)pc)) return false;
        ++ni; ++pi;
    }
    return ni == name.size();
}

static bool fnmatch_ci(const std::string& name, const std::string& pattern) {
    return glob_match_impl(name, 0, pattern, 0);
}

// ---------------------------------------------------------------- natural key
// 对应 Python util/natural.py natural_key：\d+ 切分；文本段 (0,0,casefold)、
// 数字段 (1,int,"")，tuple 逐元素比较（Python 比较 std::vector<std::tuple> 语义）。
struct NatSeg {
    int kind = 0;            // 0=文本 1=数字
    long long num = 0;
    std::string text;
};

static std::vector<NatSeg> natural_key(const std::string& s) {
    std::vector<NatSeg> out;
    size_t i = 0;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            NatSeg seg; seg.kind = 0;
            for (char c : cur) seg.text.push_back((char)std::tolower((unsigned char)c));
            out.push_back(std::move(seg));
            cur.clear();
        }
    };
    while (i < s.size()) {
        if (std::isdigit((unsigned char)s[i])) {
            flush();
            NatSeg seg; seg.kind = 1;
            size_t j = i;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) {
                // 钳制防有符号溢出（UB）：Python 用大整数，这里截断足够排序用；
                // 与 library.cpp 的同名实现保持一致（审查 L11）
                if (seg.num < (1LL << 62)) seg.num = seg.num * 10 + (s[j] - '0');
                ++j;
            }
            out.push_back(std::move(seg));
            i = j;
        } else {
            cur += s[i++];
        }
    }
    flush();
    return out;
}

static bool nat_key_less(const std::vector<NatSeg>& a, const std::vector<NatSeg>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const NatSeg& x = a[i]; const NatSeg& y = b[i];
        if (x.kind != y.kind) return x.kind < y.kind;
        if (x.kind == 1) { if (x.num != y.num) return x.num < y.num; }
        else if (x.text != y.text) return x.text < y.text;
    }
    return a.size() < b.size();
}

bool natural_key_less(const std::string& a, const std::string& b) {
    if (a == b) return false;
    return nat_key_less(natural_key(a), natural_key(b));
}

// ---------------------------------------------------------------- 过滤规则
bool should_skip_entry(const std::string& norm_name, const std::vector<std::string>& skip_globs) {
    // 与 Python comics.py 逐行对齐：./.. 是路径语义符不是隐藏名；
    // glob 匹配规范化路径与 basename，`*/X` 也匹配 basename
    std::string norm = norm_name;
    for (auto& c : norm) if (c == '\\') c = '/';
    while (!norm.empty() && norm.front() == '/') norm.erase(norm.begin());
    while (!norm.empty() && norm.back() == '/') norm.pop_back();
    if (norm.empty()) return true;
    std::vector<std::string> segments;
    size_t i = 0;
    while (i <= norm.size()) {
        size_t j = norm.find('/', i);
        if (j == std::string::npos) { segments.push_back(norm.substr(i)); break; }
        segments.push_back(norm.substr(i, j - i));
        i = j + 1;
    }
    std::string base = segments.back();
    for (size_t k = 0; k + 1 < segments.size(); ++k) {
        const std::string& seg = segments[k];
        if (!seg.empty() && seg[0] == '.' && seg != "." && seg != "..") return true;
    }
    if (!base.empty() && base[0] == '.' && base != "." && base != "..") return true;
    std::string canonical = posix_normpath(norm);
    for (const auto& pat0 : skip_globs) {
        std::string p = pat0;
        // strip
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
        if (p.empty()) continue;
        if (fnmatch_ci(canonical, p) || fnmatch_ci(base, p)) return true;
        if (p.rfind("*/", 0) == 0 && fnmatch_ci(base, p.substr(2))) return true;
    }
    return false;
}

bool is_unsafe_entry_name(const std::string& norm) {
    if (norm.empty() || norm == "." || norm == "..") return true;
    if (norm.rfind("../", 0) == 0 || norm[0] == '/') return true;
    std::string first = norm.substr(0, norm.find('/'));
    if (first.find(':') != std::string::npos) return true;
    return false;
}

bool is_image_ext(const std::string& ext) {
    // IMAGE_EXT 表在 config.cpp 定义（小写、带点）
    for (size_t i = 0; i < IMAGE_EXT_COUNT; ++i)
        if (ext == IMAGE_EXT[i]) return true;
    return false;
}

// ---------------------------------------------------------------- ZIP 名解码
// CP437 → Unicode 码点表（256 项）。必须与 Python 的 cp437 编解码逐项一致：
// 0x00-0x7F 原样（C0 控制、ASCII、U+007F），0x80-0xFF 才是图形段；
// 曾把图形段前移一位（0x00→☺、'A'→'B'），非 UTF-8 条目名兜底时会得到
// 与 Python 不同的页键，页序随之漂移（审查：cp437 解码偏差）。
static const unsigned int kCP437[256] = {
    0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

static bool plausible(const U32Text& t) {
    if (t.empty()) return false;
    for (char32_t c : t) {
        if (c == 0xFFFD) return false;
        if (c < 32 && c != U'\t') return false;
    }
    return true;
}

std::string decode_zip_name(const std::string& raw_name, unsigned flag_bits) {
    if (flag_bits & 0x800) return raw_name;  // UTF-8 标志位：原名即 UTF-8
    // miniz 给到的 char* 就是压缩包内的原始文件名字节（cp437 码点 = 字节值）
    for (const char* enc : {"utf-8", "gbk", "big5", "shift_jis"}) {
        auto t = decode_strict(raw_name, enc);
        if (t && plausible(*t)) return u32_to_utf8(*t);
    }
    // 全部失败 → cp437 兜底。Python comics.py:55-70 的最后一行是
    // `return info.filename`，也就是 zipfile 用 cp437 解出来的名字；
    // 用 latin1（字节值即码点）会得到不同的字符串（0xE9 → 'é' vs 'Θ'），
    // 而这个名字是页键与自然排序的输入 → 页序可能与 Python 不同。
    U32Text t;
    t.reserve(raw_name.size());
    for (unsigned char c : raw_name) t.push_back(static_cast<char32_t>(kCP437[c]));
    return u32_to_utf8(t);
}

// ============================================================ ZIP 容器

namespace {

struct CtxOut {
    std::string* out = nullptr;
    long long limit = 0;   // ≤0 不限
    bool too_big = false;
};

// 新版 miniz：回调返回 size_t（消费字节数），返回 0 表示中止
static size_t zip_read_cb(void* pOpaque, mz_uint64, const void* pBuf, size_t n) {
    auto* ctx = (CtxOut*)pOpaque;
    if (ctx->limit > 0 && (long long)(ctx->out->size() + n) > ctx->limit + 1) {
        // 只读到 limit+1 即判定超限（与 Python _read_stream 一致）
        size_t take = (size_t)std::max<long long>(0, ctx->limit + 1 - (long long)ctx->out->size());
        ctx->out->append((const char*)pBuf, take);
        ctx->too_big = true;
        return 0;
    }
    ctx->out->append((const char*)pBuf, n);
    return n;
}

std::string limit_error(long long limit, const std::string& name) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "压缩包条目解压后超过上限（%lldMB）: %s", limit / (1024 * 1024), name.c_str());
    return buf;
}

class ZipContainer : public Container {
public:
    explicit ZipContainer(const std::filesystem::path& path) {
        std::memset(&zip_, 0, sizeof(zip_));
        // 此版 miniz 以 UTF-8 解释文件名（mz_utf8z_to_widechar），路径必须转 UTF-8
        if (!mz_zip_reader_init_file(&zip_, path_to_utf8(path).c_str(), 0))
            throw ParseError("ZIP 容器无法打开: " + path.string());
        build();
    }
    ~ZipContainer() override { mz_zip_reader_end(&zip_); }

    const std::vector<ContainerEntry>& entries() const override { return entries_; }

    std::string read(const std::string& name, long long entry_max_bytes) override {
        std::string key = posix_normpath(name);
        auto it = index_.find(key);
        if (it == index_.end()) throw AssetNotFoundError("压缩包内不存在条目: " + name);
        CtxOut ctx;
        ctx.out = &out_buf_;
        ctx.limit = entry_max_bytes;
        out_buf_.clear();
        out_buf_.reserve((size_t)std::min<long long>(entries_[it->second].size, 64LL << 20));
        // 注意：entries_ 是**过滤后**的列表（跳过目录条目/不安全名/重名），而 miniz 的
        // extract 需要压缩包全局文件号 → 必须用并行保存的 zip_index_，不能用 entries_ 下标。
        mz_uint idx = (mz_uint)zip_index_[it->second];
        if (!mz_zip_reader_extract_to_callback(&zip_, idx, zip_read_cb, &ctx, 0) || ctx.too_big) {
            out_buf_.clear();
            if (ctx.too_big && entry_max_bytes > 0) throw ParseError(limit_error(entry_max_bytes, key));
            throw ParseError("条目读取失败 " + name);
        }
        return std::move(out_buf_);
    }

private:
    void build() {
        unsigned n = mz_zip_reader_get_num_files(&zip_);
        std::map<std::string, std::pair<std::string, size_t>> seen;  // norm → (解码原名, entries 下标)
        for (unsigned i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip_, i, &st)) continue;
            if (mz_zip_reader_is_file_a_directory(&zip_, i)) continue;  // 目录条目跳过
            std::string raw = st.m_filename;
            std::string name = decode_zip_name(raw, st.m_bit_flag);
            for (auto& c : name) if (c == '\\') c = '/';
            std::string norm = posix_normpath(name);
            if (is_unsafe_entry_name(norm)) continue;
            auto it = seen.find(norm);
            if (it != seen.end()) {
                // 规范化后重名：保留 natural_key_less 较小者
                const std::string& prev = it->second.first;
                const std::string& cur = name;
                if (!natural_key_less(cur, prev)) continue;
                entries_[it->second.second] = {norm, st.m_uncomp_size, st.m_comp_size, (double)st.m_time};
                zip_index_[it->second.second] = i;   // 同步替换压缩包全局文件号
                it->second.first = cur;
                continue;
            }
            seen[norm] = {name, entries_.size()};
            entries_.push_back({norm, st.m_uncomp_size, st.m_comp_size, (double)st.m_time});
            zip_index_.push_back(i);                 // entries_ 与 zip_index_ 严格同序
        }
        for (size_t i = 0; i < entries_.size(); ++i) index_[entries_[i].name] = i;
    }

    mz_zip_archive zip_{};
    std::vector<ContainerEntry> entries_;
    std::vector<size_t> zip_index_;   // entries_[k] 对应的压缩包全局文件号
    std::map<std::string, size_t> index_;
    std::string out_buf_;  // 复用缓冲（单适配器单线程约定）
};

// ============================================================ 目录容器

class DirContainer : public Container {
public:
    explicit DirContainer(const std::filesystem::path& path, bool recursive = false)
        : root_(path) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root_, ec)) throw ParseError("不是目录: " + root_.string());
        // 非递归：只列顶层；递归（gallery.recursive / open-folder recursive=true）：
        // 按 os.walk 语义下钻，条目名为相对路径（'/' 分隔）。
        // 符号链接一律跳过：Python comics.py:531/545 用 entry.is_symlink() 排除，
        // 而 is_regular_file 会跟随链接 → 图集页数比列表里报的多一张重复图。
        auto add = [&](const std::filesystem::path& p, const std::string& name) {
            std::error_code ec2;
            if (std::filesystem::is_symlink(p, ec2)) return;
            if (!std::filesystem::is_regular_file(p, ec2) || ec2) return;
            if (is_unsafe_entry_name(name)) return;
            auto sz = (unsigned long long)std::filesystem::file_size(p, ec2);
            if (ec2) sz = 0;
            entries_.push_back({name, sz, sz, 0.0});
        };
        if (!recursive) {
            for (auto it = std::filesystem::directory_iterator(root_, ec);
                 it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                add(it->path(), path_to_utf8(it->path().filename()));
            }
        } else {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root_, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                std::error_code ec2;
                // junction 不被 is_symlink 识别（MSVC），漏判会随递归成环栈溢出
                if (it->is_directory(ec2) &&
                    (std::filesystem::is_symlink(it->path(), ec2) ||
                     is_symlink_or_junction(it->path()))) {
                    it.disable_recursion_pending();
                    continue;
                }
                auto rel = std::filesystem::relative(it->path(), root_, ec2);
                if (ec2) continue;
                add(it->path(), path_to_utf8(rel.generic_wstring()));
            }
        }
    }

    const std::vector<ContainerEntry>& entries() const override { return entries_; }

    std::string read(const std::string& name, long long entry_max_bytes) override {
        std::error_code ec1, ec2;
        auto target = std::filesystem::weakly_canonical(root_ / std::filesystem::u8path(name), ec1);
        auto rootc = std::filesystem::weakly_canonical(root_, ec2);
        // 越出根目录的请求一律拒绝。必须做**分隔符边界**的前缀比较：
        // 子串 find 会把 C:\books2 当成 C:\books 的子路径（审查 L12），
        // 且两个调用各有自己的 error_code，不能只看后一个。
        // canonical 化已按路径分量解析 ".."，这里不再做子串检查：否则
        // img..jpg 这类合法名字也会被误拒（审查：DirContainer 子串拒 ..）。
        bool inside = false;
        if (!ec1 && !ec2) {
            auto ts = target.wstring(), rs = rootc.wstring();
            inside = ts == rs ||
                     (ts.size() > rs.size() && ts.compare(0, rs.size(), rs) == 0 &&
                      (ts[rs.size()] == L'\\' || ts[rs.size()] == L'/'));
        }
        if (!inside)
            throw AssetNotFoundError("非法条目: " + name);
        std::ifstream in(target, std::ios::binary);
        if (!in) throw ParseError("图片读取失败 " + name);
        if (entry_max_bytes > 0) {
            in.seekg(0, std::ios::end);
            long long sz = (long long)in.tellg();
            if (sz > entry_max_bytes) throw ParseError(limit_error(entry_max_bytes, name));
            in.seekg(0);
        }
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return data;
    }

private:
    std::filesystem::path root_;
    std::vector<ContainerEntry> entries_;
};

// ============================================================ TAR 容器

class TarContainer : public Container {
    struct Item {
        std::string name;
        unsigned long long size = 0;
        unsigned long long offset = 0;  // 数据起始偏移
        double mtime = 0;
    };

public:
    explicit TarContainer(const std::filesystem::path& path) : path_(path) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) throw ParseError("TAR 容器无法打开: " + path_.string());
        std::string pending_long;
        std::map<std::string, std::string> global_pax, file_pax;
        unsigned long long off = 0;
        while (true) {
            char hdr[512];
            in.read(hdr, 512);
            if (in.gcount() < 512) break;
            off += 512;
            bool all_zero = true;
            for (char c : hdr) if (c != '\0') { all_zero = false; break; }
            if (all_zero) break;  // 结束块
            std::string name(hdr, strnlen(hdr, 100));
            unsigned long long size = parse_number(hdr + 124, 12);
            char typeflag = hdr[156];
            std::string prefix(hdr + 345, strnlen(hdr + 345, 155));
            if (std::strncmp(hdr + 257, "ustar", 5) == 0 && !prefix.empty())
                name = prefix + "/" + name;
            unsigned long long data_off = off;
            if (typeflag == 'x' || typeflag == 'g') {
                // POSIX pax 扩展头：数据是 "<长度> key=value\n" 记录流（长度含自身）。
                // Python tarfile 全局（g）与单文件（x）都支持，path/size 覆盖 ustar 头
                // （长路径、超 8G 尺寸），不解析会让这些条目名/偏移全错。
                if (size > kMaxPaxBytes) throw ParseError("TAR pax 头过大");
                std::string pax_data((size_t)size, '\0');
                if (size) {
                    in.seekg((long long)data_off);
                    in.read(pax_data.data(), (std::streamsize)size);
                }
                unsigned long long pad = (512 - (size % 512)) % 512;
                off += size + pad;
                parse_pax(pax_data, typeflag == 'g' ? global_pax : file_pax);
                in.seekg((long long)off);
                continue;
            }
            // 单文件 pax 优先于全局 pax（与 tarfile 的合并顺序一致）
            auto pick = [&](const char* key) -> const std::string* {
                auto it = file_pax.find(key);
                if (it != file_pax.end()) return &it->second;
                it = global_pax.find(key);
                return it != global_pax.end() ? &it->second : nullptr;
            };
            if (const std::string* p = pick("path")) name = *p;
            if (const std::string* p = pick("size")) size = parse_decimal(*p, size);
            file_pax.clear();  // pax 记录只作用于紧随其后的一个条目
            // size 向上对齐到 512
            unsigned long long pad = (512 - (size % 512)) % 512;
            off += size + pad;
            if (typeflag == 'L') {
                // GNU 长名：数据块内容是下一个条目的真实名字
                std::string longname((size_t)size, '\0');
                in.seekg((long long)data_off);
                in.read(longname.data(), (std::streamsize)size);
                pending_long = longname.substr(0, longname.find('\0'));
                // 必须把读指针复位到下一个 512 块头（off），否则会从长名数据中间
                // 继续解析，后面所有条目全乱（GNU tar 写长名时 size/pad 会移动 off，
                // 不复位就再也对不齐）
                in.seekg((long long)off);
                continue;
            }
            if (!pending_long.empty()) { name = pending_long; pending_long.clear(); }
            if (typeflag != '0' && typeflag != '\0') continue;  // 目录/链接等跳过
            in.seekg((long long)off);
            for (auto& c : name) if (c == '\\') c = '/';
            std::string norm = posix_normpath(name);
            if (is_unsafe_entry_name(norm)) continue;
            double mt = (double)parse_number(hdr + 136, 12);
            items_.push_back({norm, size, data_off, mt});
        }
        for (size_t i = 0; i < items_.size(); ++i) index_[items_[i].name] = i;
    }

    const std::vector<ContainerEntry>& entries() const override {
        if (ent_.empty() && !items_.empty())
            for (const auto& it : items_) ent_.push_back({it.name, it.size, it.size, it.mtime});
        return ent_;
    }

    std::string read(const std::string& name, long long entry_max_bytes) override {
        std::string key = posix_normpath(name);
        auto it = index_.find(key);
        if (it == index_.end()) throw AssetNotFoundError("TAR 内不存在条目: " + name);
        const Item& item = items_[it->second];
        std::ifstream in(path_, std::ios::binary);
        if (!in) throw ParseError("TAR 条目读取失败 " + name);
        in.seekg((long long)item.offset);
        long long want = item.size > (unsigned long long)std::numeric_limits<long long>::max()
                             ? std::numeric_limits<long long>::max()
                             : (long long)item.size;
        if (entry_max_bytes > 0 && want > entry_max_bytes) {
            // 流式强制：只读 limit+1 判定
            std::string head((size_t)std::min<long long>(want, entry_max_bytes + 1), '\0');
            in.read(head.data(), (std::streamsize)head.size());
            throw ParseError(limit_error(entry_max_bytes, key));
        }
        // 不限长时也不能按声明尺寸盲目分配：截断/撒谎的头（base-256 可声明超大）
        // 会让分配量远超实际数据；Python fh.read() 也是读到 EOF 即止
        in.seekg(0, std::ios::end);
        long long fsize = (long long)in.tellg();
        if (fsize >= 0) {
            if (item.offset > (unsigned long long)fsize) want = 0;
            else {
                long long avail = fsize - (long long)item.offset;
                if (avail < want) want = avail < 0 ? 0 : avail;
            }
        }
        in.seekg((long long)item.offset);
        std::string data((size_t)want, '\0');
        in.read(data.data(), (std::streamsize)want);
        data.resize((size_t)in.gcount());
        return data;
    }

private:
    // pax 头数据上限：正常只有几个字段；纯防御畸形头撑内存
    static constexpr unsigned long long kMaxPaxBytes = 16ULL << 20;

    // ustar 数值字段：八进制（空格/NUL 终止）或 GNU base-256（首字节最高位置 1，
    // 其余位为大端二进制；负数对 size/mtime 无意义，按无符号饱和处理）。
    static unsigned long long parse_number(const char* p, size_t n) {
        if (n == 0) return 0;
        if ((unsigned char)p[0] & 0x80) {
            unsigned long long v = (unsigned char)p[0] & 0x7F;
            for (size_t i = 1; i < n; ++i) {
                unsigned char b = (unsigned char)p[i];
                v = v > (std::numeric_limits<unsigned long long>::max() >> 8)
                        ? std::numeric_limits<unsigned long long>::max()
                        : ((v << 8) | b);
            }
            return v;
        }
        return parse_octal(p, n);
    }

    static unsigned long long parse_octal(const char* p, size_t n) {
        unsigned long long v = 0;
        for (size_t i = 0; i < n; ++i) {
            char c = p[i];
            if (c == '\0' || c == ' ') { if (v) break; else continue; }
            if (c < '0' || c > '7') break;
            v = v * 8 + (unsigned)(c - '0');
        }
        return v;
    }

    // pax size 字段是十进制文本；非法/溢出返回 fallback（保留 ustar 头里的值）
    static unsigned long long parse_decimal(const std::string& s, unsigned long long fallback) {
        if (s.empty()) return fallback;
        unsigned long long v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return fallback;
            if (v > (std::numeric_limits<unsigned long long>::max() - 9) / 10) return fallback;
            v = v * 10 + (unsigned)(c - '0');
        }
        return v;
    }

    // pax 记录流：每条为 "<十进制长度> <key>=<value>\n"（长度为整条记录的字节数）
    static void parse_pax(const std::string& data, std::map<std::string, std::string>& out) {
        size_t i = 0;
        while (i < data.size()) {
            size_t sp = data.find(' ', i);
            if (sp == std::string::npos) break;
            unsigned long long len = 0;
            bool ok = sp > i;
            for (size_t k = i; ok && k < sp; ++k) {
                char c = data[k];
                if (c < '0' || c > '9') { ok = false; break; }
                len = len * 10 + (unsigned)(c - '0');
            }
            if (!ok || len < sp - i + 2 || (unsigned long long)(data.size() - i) < len) break;
            size_t eq = data.find('=', sp + 1);
            if (eq == std::string::npos || eq >= i + (size_t)len) break;
            std::string key = data.substr(sp + 1, eq - sp - 1);
            size_t vend = i + (size_t)len;
            if (vend > eq + 1 && data[vend - 1] == '\n') --vend;
            out[key] = data.substr(eq + 1, vend - eq - 1);
            i += (size_t)len;
        }
    }

    std::filesystem::path path_;
    std::vector<Item> items_;
    mutable std::vector<ContainerEntry> ent_;  // entries() const 内惰性填充
    std::map<std::string, size_t> index_;
};

// ============================================================ 外部工具（RAR/7z）
// find_tool_in_path / quote_windows_arg 见 util/paths.h（container.cpp 与 mobi.cpp 共用）。

// 列表/条目名是 UTF-8（工具以 UTF-8 输出）→ CreateProcessW 需要宽命令行
static std::wstring u8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// 单次外部命令的最长等待。Python rarfile/py7zr 没有显式超时，但 C++ 是线程池模型：
// 卡死的 unrar/7z 会永久占住工作线程；到期杀整棵进程树再等退出（审查 L2）。
static constexpr DWORD kToolTimeoutMs = 120 * 1000;
// 列表输出上限：只是条目名文本，正常远小于此（纯防御畸形/海量条目）
static constexpr long long kMaxListingBytes = 64LL << 20;

struct ToolRun {
    int code = -1;           // -1 = 启动失败/被中止
    bool limit_hit = false;  // 输出达到 max_out，已杀进程
    bool timed_out = false;  // 超时，已杀进程
};

// RAII 句柄：任何 return/异常路径都不泄漏内核句柄（旧实现超时时没有等进程退出，
// 析构清理会与存活子进程竞态）
struct WinHandle {
    HANDLE h = nullptr;
    WinHandle() = default;
    explicit WinHandle(HANDLE x) : h(x) {}
    ~WinHandle() { close_(); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    WinHandle& operator=(WinHandle&& o) noexcept {
        if (this != &o) { close_(); h = o.h; o.h = nullptr; }
        return *this;
    }
    void reset(HANDLE x = nullptr) { close_(); h = x; }
    HANDLE get() const { return h; }
    explicit operator bool() const { return h != nullptr && h != INVALID_HANDLE_VALUE; }

private:
    void close_() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
};

// 运行命令行并捕获 stdout（stderr 与无需捕获时丢 NUL）。CreateProcessW + Job Object：
// 超时或输出超限时终止整棵进程树，等进程退出后再由 RAII 关闭句柄。
// max_out > 0 时最多接受 max_out 字节即中止（调用方按 Python 的 limit+1 语义判定）。
static ToolRun run_tool(const std::wstring& cmdline, std::string* out, long long max_out,
                        DWORD timeout_ms) {
    ToolRun res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    WinHandle rd, wr, nul_out, nul_in, job;
    if (out) {
        HANDLE r = nullptr, w = nullptr;
        if (!CreatePipe(&r, &w, &sa, 1 << 16)) return res;
        rd.reset(r);
        wr.reset(w);
        SetHandleInformation(rd.get(), HANDLE_FLAG_INHERIT, 0);
    }
    nul_out.reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    nul_in.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr));
    if (!nul_out || !nul_in) return res;
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul_in.get();  // 后端若想读 stdin（口令询问）立即 EOF，不挂线程
    si.hStdOutput = out ? wr.get() : nul_out.get();
    si.hStdError = nul_out.get();
    std::wstring mut = cmdline;  // CreateProcessW 需要可写缓冲
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return res;
    WinHandle hp(pi.hProcess), ht(pi.hThread);
    wr.reset();  // 父进程必须关掉写端，否则 ReadFile 等不到 EOF
    const bool in_job = job && AssignProcessToJobObject(job.get(), hp.get());
    ResumeThread(ht.get());
    ht.reset();

    const DWORD start = GetTickCount();
    if (out) {
        out->clear();
        char buf[1 << 16];
        // 用 PeekNamedPipe 轮询而不是阻塞式 ReadFile：卡死的后端即使不关 stdout
        // （或留下持有管道写端的孙进程）也能被总超时约束，线程不会被无限挂住
        while (true) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd.get(), nullptr, 0, nullptr, &avail, nullptr)) break;  // 写端已关
            if (avail > 0) {
                if (GetTickCount() - start >= timeout_ms) {  // 持续出数据也要受总时长约束
                    res.timed_out = true;
                    break;
                }
                DWORD got = 0;
                if (!ReadFile(rd.get(), buf, sizeof(buf), &got, nullptr) || got == 0) break;
                if (max_out > 0 && (long long)(out->size() + got) > max_out) {
                    size_t take = (size_t)std::max<long long>(0, max_out - (long long)out->size());
                    out->append(buf, take);
                    res.limit_hit = true;
                    break;
                }
                out->append(buf, got);
                continue;
            }
            if (WaitForSingleObject(hp.get(), 10) == WAIT_OBJECT_0) break;
            if (GetTickCount() - start >= timeout_ms) {
                res.timed_out = true;
                break;
            }
        }
        rd.reset();
    }
    // 杀整棵进程树；Assign 失败（嵌套 Job 受限）退回 taskkill /T /F，再兜底直接杀
    auto kill_tree = [&]() {
        if (in_job) {
            TerminateJobObject(job.get(), 1);
            return;
        }
        std::wstring cmd = L"taskkill /PID " + std::to_wstring(pi.dwProcessId) + L" /T /F";
        STARTUPINFOW si2{};
        si2.cb = sizeof(si2);
        si2.dwFlags = STARTF_USESHOWWINDOW;
        si2.wShowWindow = SW_HIDE;
        std::wstring mut2 = cmd;
        PROCESS_INFORMATION pi2{};
        if (CreateProcessW(nullptr, mut2.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si2, &pi2)) {
            WaitForSingleObject(pi2.hProcess, 10 * 1000);
            CloseHandle(pi2.hThread);
            CloseHandle(pi2.hProcess);
        }
        TerminateProcess(hp.get(), 1);
    };
    if (res.limit_hit || res.timed_out) {
        kill_tree();
        WaitForSingleObject(hp.get(), 30 * 1000);  // 等退出后再关句柄，避免残留子进程
    } else if (out) {
        // stdout 已关，进程应当已退出；给少量时间回收（极端情况下 job 析构会兜底杀）
        WaitForSingleObject(hp.get(), 10 * 1000);
    } else if (WaitForSingleObject(hp.get(), timeout_ms) == WAIT_TIMEOUT) {
        res.timed_out = true;
        kill_tree();
        WaitForSingleObject(hp.get(), 30 * 1000);
    }
    DWORD code = 1;
    GetExitCodeProcess(hp.get(), &code);
    res.code = (res.limit_hit || res.timed_out) ? -1 : (int)code;
    return res;  // job 析构（KILL_ON_JOB_CLOSE）顺带清掉任何存活的后代进程
}

// 外部工具容器（RAR/7z）：构造时只列表一次（名字/尺寸），读取时按需把**单个条目**
// 解到内存，全程按 max_archive_entry_mb / max_archive_total_mb 限长（超限即杀进程）。
// 不再整包解压到临时目录：炸弹不会先撑满临时盘，每个 /page 请求也只解它要的页
// （对应 Python rarfile/py7zr 的按条目读取语义）。
class ExternalContainer : public Container {
public:
    ExternalContainer(std::string backend, const std::filesystem::path& archive)
        : backend_(std::move(backend)), archive_(archive) {
        // 列表要起外部进程（重活）；可重入，外层适配器构造已持闸门时不会重复占槽
        auto heavy = heavy_gate().acquire();
        if (backend_ == "rar") {
            unrar_ = find_tool_in_path(L"unrar");
            if (unrar_.empty()) bsdtar_ = find_tool_in_path(L"bsdtar");
            if (unrar_.empty() && bsdtar_.empty())
                throw UnsupportedFormatError("需要 RAR 后端", "需安装 unrar");
            if (!unrar_.empty()) load_rar_listing();
            else load_bsdtar_listing();
        } else {
            seven_ = find_tool_in_path(L"7z");
            if (seven_.empty()) throw UnsupportedFormatError("需要 7z 后端", "需安装 7-Zip");
            load_7z_listing();
        }
        for (size_t i = 0; i < entries_.size(); ++i) index_[entries_[i].name] = i;
    }

    // comics 层在 build_names 之前传入 max_archive_total_mb（字节；0 = 不限）
    void set_total_limit(long long bytes) override { total_limit_ = bytes; }

    const std::vector<ContainerEntry>& entries() const override { return entries_; }

    std::string read(const std::string& name, long long entry_max_bytes) override {
        // 每次读取都起一个外部进程（unrar/bsdtar/7z）：按重活闸门限流
        auto heavy = heavy_gate().acquire();
        std::string key = posix_normpath(name);
        auto it = index_.find(key);
        if (it == index_.end())
            throw AssetNotFoundError(backend_ == "rar" ? "RAR 内不存在条目: " + name
                                                       : "7z 内不存在条目: " + name);
        const std::string& raw = raws_[it->second];
        // 同一适配器实例内的重复读取（probe 尺寸后再取页/切片）命中缓存，不再起
        // 子进程；缓存字节不重复计入 total_read_（总量限的是实际解压量）。RAR 的
        // “每条目一次 unrar 进程”与 Python rarfile 的 PipeReader 逐文件进程语义一致。
        auto cached = cache_.find(key);
        if (cached != cache_.end()) {
            if (entry_max_bytes > 0 && (long long)cached->second.size() > entry_max_bytes)
                throw ParseError(limit_error(entry_max_bytes, key));
            return cached->second;
        }
        long long cap = 0;  // 0 = 不限
        bool cap_is_total = false;
        if (entry_max_bytes > 0) {
            // 与 Python _read_stream 一致：读 limit+1 判超；恰好读到 limit+1 字节时
            // run_tool 不置 limit_hit（追加后未超过 max_out），返回后必须用 data.size() 复核
            cap = entry_max_bytes + 1;
        } else if (total_limit_ > 0) {
            cap = std::max<long long>(1, total_limit_ + 1 - total_read_);
            cap_is_total = true;
        }
        std::string data;
        ToolRun r = run_tool(build_read_cmd(raw), &data, cap, kToolTimeoutMs);
        const char* what = backend_ == "rar" ? "RAR" : "7z";
        if (r.timed_out) throw ParseError(std::string(what) + " 条目读取超时（已终止后端）: " + name);
        if (r.limit_hit) {
            if (cap_is_total)
                throw ParseError(archive_total_limit_error(total_read_ + (long long)data.size()));
            throw ParseError(limit_error(entry_max_bytes, key));
        }
        if (r.code != 0) throw ParseError(std::string(what) + " 条目读取失败: " + name);
        if (entry_max_bytes > 0 && (long long)data.size() > entry_max_bytes)
            throw ParseError(limit_error(entry_max_bytes, key));
        total_read_ += (long long)data.size();
        if (total_limit_ > 0 && total_read_ > total_limit_)
            throw ParseError(archive_total_limit_error(total_read_));
        if (cache_bytes_ + (long long)data.size() <= kCacheBudget) {
            cache_bytes_ += (long long)data.size();
            cache_.emplace(key, data);
        }
        return data;
    }

private:
    // 单适配器实例的条目缓存上限：避免 probe 与取页对同一条目重复解压，同时防止
    // 一个请求把整包常驻内存。适配器按请求创建，缓存生命周期与请求一致。
    static constexpr long long kCacheBudget = 32LL * 1024 * 1024;
    std::map<std::string, std::string> cache_;
    long long cache_bytes_ = 0;
    static bool starts_at(const std::string& s, size_t pos, const char* prefix) {
        size_t n = std::strlen(prefix);
        return pos <= s.size() && s.size() - pos >= n &&
               std::memcmp(s.data() + pos, prefix, n) == 0;
    }

    // 列表行 → 条目：名字解码（与 ZIP 同一启发式）、'\'→'/'、normpath、越界过滤；
    // raw 原样保留（读取时要回传给工具），与 entries_ 严格同序。
    void add_entry(const std::string& raw0, unsigned long long size) {
        std::string raw = raw0;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
        if (raw.empty()) return;
        std::string name = decode_zip_name(raw, 0);
        for (auto& c : name) if (c == '\\') c = '/';
        std::string norm = posix_normpath(name);
        if (is_unsafe_entry_name(norm)) return;
        entries_.push_back({norm, size, size, 0.0});
        raws_.push_back(raw);
    }

    void load_rar_listing() {
        // lt = 技术列表，每块 Name/Type/Size 各一行（-scf 让名字按 UTF-8 输出，
        // -c- 不显示注释以免注释行伪造条目）；-- 停止开关扫描（rarfile 同款）。
        std::wstring cmd = quote_windows_arg(unrar_) + L" lt -p- -scf -c- -- " +
                           quote_windows_arg(archive_.wstring());
        std::string out;
        ToolRun r = run_tool(cmd, &out, kMaxListingBytes, kToolTimeoutMs);
        if (r.code != 0 || r.limit_hit) throw ParseError("RAR 容器无法打开（列表失败）");
        std::string raw, type;
        unsigned long long size = 0;
        bool has = false;
        auto flush = [&]() {
            if (has && type != "Directory") add_entry(raw, size);
            has = false;
        };
        size_t i = 0;
        while (i <= out.size()) {
            size_t j = out.find('\n', i);
            bool last = j == std::string::npos;
            if (last) j = out.size();
            std::string line = out.substr(i, j - i);
            i = j + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();  // Windows 工具输出
            size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos) {
                if (starts_at(line, b, "Name: ")) {
                    flush();
                    raw = line.substr(b + 6);
                    type.clear();
                    size = 0;
                    has = true;
                } else if (has && starts_at(line, b, "Type: ")) {
                    type = line.substr(b + 6);
                } else if (has && starts_at(line, b, "Size: ")) {
                    size = parse_size(line.substr(b + 6));
                }
            }
            if (last) break;
        }
        flush();
    }

    void load_bsdtar_listing() {
        // bsdtar 的 -tf 只有名字（目录以 / 结尾）；尺寸由读取路径累计限长兜底
        std::wstring cmd = quote_windows_arg(bsdtar_) + L" -tf " + quote_windows_arg(archive_.wstring());
        std::string out;
        ToolRun r = run_tool(cmd, &out, kMaxListingBytes, kToolTimeoutMs);
        if (r.code != 0 || r.limit_hit) throw ParseError("RAR 容器无法打开（列表失败）");
        size_t i = 0;
        while (i < out.size()) {
            size_t j = out.find('\n', i);
            if (j == std::string::npos) j = out.size();
            std::string line = out.substr(i, j - i);
            i = j + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.back() == '/') continue;
            add_entry(line, 0);
        }
    }

    void load_7z_listing() {
        // -slt：每条目一个 "Path = ..." 块；-sccUTF-8 固定名字编码；-p 空口令防询问；
        // 包头块带 Type 行，按此跳过；目录按 Attributes 首字符 D（或 Folder = +）。
        std::wstring cmd = quote_windows_arg(seven_) + L" l -slt -sccUTF-8 -p -- " +
                           quote_windows_arg(archive_.wstring());
        std::string out;
        ToolRun r = run_tool(cmd, &out, kMaxListingBytes, kToolTimeoutMs);
        if (r.code != 0 || r.limit_hit) throw ParseError("7z 容器无法打开（列表失败）");
        std::string path;
        unsigned long long size = 0;
        bool has = false, is_dir = false, is_header = false;
        auto flush = [&]() {
            if (has && !is_header && !is_dir) add_entry(path, size);
            has = false;
            is_dir = false;
            is_header = false;
        };
        size_t i = 0;
        while (i <= out.size()) {
            size_t j = out.find('\n', i);
            bool last = j == std::string::npos;
            if (last) j = out.size();
            std::string line = out.substr(i, j - i);
            i = j + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();  // Windows 工具输出
            size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos) {
                if (starts_at(line, b, "Path = ")) {
                    flush();
                    path = line.substr(b + 7);
                    size = 0;
                    has = true;
                } else if (has) {
                    if (starts_at(line, b, "Type = ")) is_header = true;
                    else if (starts_at(line, b, "Size = ")) size = parse_size(line.substr(b + 7));
                    else if (starts_at(line, b, "Attributes = "))
                        is_dir = line.size() > b + 13 && line[b + 13] == 'D';
                    else if (starts_at(line, b, "Folder = +")) is_dir = true;
                }
            }
            if (last) break;
        }
        flush();
    }

    // 列表里的 Size 字段：非法即 0；截断防累计求和溢出（声明尺寸不可全信）
    static unsigned long long parse_size(const std::string& s) {
        unsigned long long v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') break;
            v = v * 10 + (unsigned)(c - '0');
            if (v > (1ULL << 62)) return 1ULL << 62;
        }
        return v;
    }

    // 读取命令与 Python rarfile 的 open_cmd 逐参数对齐：
    // unrar  p -inul -p- -- archive name
    // bsdtar -x --to-stdout -f archive name
    // 7z     e -so -bb0 -p -- archive name
    // rarfile 会把 "/" 换成 os.path.sep 再传（unrar 掩码要求本机分隔符），Win 上即 "\"
    std::wstring build_read_cmd(const std::string& raw) const {
        std::string mask = raw;
        for (auto& c : mask)
            if (c == '/') c = '\\';
        std::wstring wname = u8_to_wide(mask);
        if (backend_ == "rar") {
            if (!unrar_.empty())
                return quote_windows_arg(unrar_) + L" p -inul -p- -- " + quote_windows_arg(archive_.wstring()) +
                       L" " + quote_windows_arg(wname);
            return quote_windows_arg(bsdtar_) + L" -x --to-stdout -f " + quote_windows_arg(archive_.wstring()) +
                   L" " + quote_windows_arg(wname);
        }
        return quote_windows_arg(seven_) + L" e -so -bb0 -p -- " + quote_windows_arg(archive_.wstring()) + L" " +
               quote_windows_arg(wname);
    }

    std::string backend_;
    std::filesystem::path archive_;
    std::wstring unrar_, bsdtar_, seven_;
    std::vector<ContainerEntry> entries_;
    std::vector<std::string> raws_;  // entries_[i] 对应的工具可识别原始名
    std::map<std::string, size_t> index_;
    long long total_limit_ = 0;  // max_archive_total_mb（字节；0 = 不限）
    long long total_read_ = 0;   // 本容器生命周期内的累计解压字节
};

}  // namespace

// ============================================================ 工厂

std::unique_ptr<Container> open_zip(const std::filesystem::path& path) {
    return std::make_unique<ZipContainer>(path);
}

std::unique_ptr<Container> open_dir(const std::filesystem::path& path, bool recursive) {
    return std::make_unique<DirContainer>(path, recursive);
}

std::unique_ptr<Container> open_tar(const std::filesystem::path& path) {
    return std::make_unique<TarContainer>(path);
}

std::unique_ptr<Container> open_rar(const std::filesystem::path& path) {
    // 缺失工具的判定放在容器构造前，保证"找不到工具必须抛"的契约
    if (find_tool_in_path(L"unrar").empty() && find_tool_in_path(L"bsdtar").empty())
        throw UnsupportedFormatError("需要 RAR 后端", "需安装 unrar");
    return std::make_unique<ExternalContainer>("rar", path);
}

std::unique_ptr<Container> open_7z(const std::filesystem::path& path) {
    if (find_tool_in_path(L"7z").empty())
        throw UnsupportedFormatError("需要 7z 后端", "需安装 7-Zip");
    return std::make_unique<ExternalContainer>("7z", path);
}

// 探测结果缓存：目录列表里每个条目都会问一次 openable，不缓存等于每次列目录
// 都做几百次 PATH 扫描（Python registry.py:139-155 同样缓存）。
bool rar_backend_available() {
    static const bool ok = [] { return !find_tool_in_path(L"unrar").empty() || !find_tool_in_path(L"bsdtar").empty(); }();
    return ok;
}

bool seven_backend_available() {
    static const bool ok = [] { return !find_tool_in_path(L"7z").empty(); }();
    return ok;
}

}  // namespace lnv
