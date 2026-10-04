// 容器实现（对应 Python formats/comics.py 的容器层 + zipfile/tarfile 语义）
// ZIP 用 miniz；TAR 自写 ustar 读取；RAR/7z 借外部工具整包解压到临时目录后走目录语义。
#include "lnv/formats/container.h"
#include "lnv/util/textutil.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <fstream>
#include <cstring>

#include <windows.h>
#include <miniz.h>

namespace lnv {
namespace { std::string path_to_utf8(const std::filesystem::path& p) {
    auto w = p.wstring();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;  // 此版 miniz 以 UTF-8 解释文件名（mz_utf8z_to_widechar）
} }


// ============================================================ 小工具

// std::filesystem::path → UTF-8 窄串（C++20 u8string 是 char8_t）
static std::string path_u8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

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
// CP437 → Unicode 码点表（256 项）
static const unsigned int kCP437[256] = {
    0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,0x25BA,
    0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,0x0020,
    0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,0x0030,
    0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,0x0040,
    0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,0x0050,
    0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,0x0060,
    0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,0x0070,
    0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x2302,0x263A,
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
                add(it->path(), path_u8(it->path().filename()));
            }
        } else {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root_, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                std::error_code ec2;
                if (it->is_directory(ec2) && std::filesystem::is_symlink(it->path(), ec2)) {
                    it.disable_recursion_pending();
                    continue;
                }
                auto rel = std::filesystem::relative(it->path(), root_, ec2);
                if (ec2) continue;
                add(it->path(), path_u8(rel.generic_wstring()));
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
        bool inside = false;
        if (!ec1 && !ec2) {
            auto ts = target.wstring(), rs = rootc.wstring();
            inside = ts == rs ||
                     (ts.size() > rs.size() && ts.compare(0, rs.size(), rs) == 0 &&
                      (ts[rs.size()] == L'\\' || ts[rs.size()] == L'/'));
        }
        if (!inside || name.find("..") != std::string::npos)
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
            unsigned long long size = parse_octal(hdr + 124, 12);
            char typeflag = hdr[156];
            std::string prefix(hdr + 345, strnlen(hdr + 345, 155));
            if (std::strncmp(hdr + 257, "ustar", 5) == 0 && !prefix.empty())
                name = prefix + "/" + name;
            unsigned long long data_off = off;
            // size 向上对齐到 512
            unsigned long long pad = (512 - (size % 512)) % 512;
            off += size + pad;
            if (typeflag == 'L') {
                // GNU 长名：数据块内容是下一个条目的真实名字
                std::string longname(size, '\0');
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
            if (typeflag != '0' && typeflag != '\0') continue;  // 目录/链接/pax 头等跳过（简配）
            in.seekg((long long)off);
            for (auto& c : name) if (c == '\\') c = '/';
            std::string norm = posix_normpath(name);
            if (is_unsafe_entry_name(norm)) continue;
            double mt = (double)parse_octal(hdr + 136, 12);
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
        long long want = (long long)item.size;
        if (entry_max_bytes > 0 && want > entry_max_bytes) {
            // 流式强制：只读 limit+1 判定
            std::string head((size_t)std::min<long long>(want, entry_max_bytes + 1), '\0');
            in.read(head.data(), (std::streamsize)head.size());
            throw ParseError(limit_error(entry_max_bytes, key));
        }
        std::string data((size_t)want, '\0');
        in.read(data.data(), (std::streamsize)want);
        data.resize((size_t)in.gcount());
        return data;
    }

private:
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

    std::filesystem::path path_;
    std::vector<Item> items_;
    mutable std::vector<ContainerEntry> ent_;  // entries() const 内惰性填充
    std::map<std::string, size_t> index_;
};

// ============================================================ 外部工具（RAR/7z）

// 在 PATH（以及系统默认搜索序）上定位工具；找不到返回空
static std::string find_tool(const char* name) {
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, name, ".exe", MAX_PATH, buf, nullptr) > 0) return buf;
    return {};
}

// 执行命令并捕获 stdout（stderr 丢到 NUL）；返回 exit code，-1 = 启动失败
static int run_capture(const std::string& cmdline, std::string& out) {
    std::string cmd_mutable = cmdline;  // CreateProcessA 需要可变缓冲
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = nul ? nul : wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd_mutable.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// 执行命令（输出全部丢 NUL）；返回 exit code，-1 = 启动失败
static int run_quiet(const std::string& cmdline) {
    std::string cmd_mutable = cmdline;  // CreateProcessA 需要可变缓冲
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (!nul) return -1;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = nul;
    si.hStdError = nul;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd_mutable.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(nul);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

// 整包解压到临时目录后递归枚举（任务允许的简化实现）
class ExtractedContainer : public Container {
public:
    ExtractedContainer(std::string backend, const std::filesystem::path& archive)
        : backend_(std::move(backend)) {
        // 临时目录：独立命名，避免并发冲突
        static std::atomic<unsigned> seq{0};
        char name[64];
        std::snprintf(name, sizeof(name), "lnvx_%lu_%u", (unsigned long)GetCurrentProcessId(), seq.fetch_add(1));
        std::error_code ec;
        dest_ = std::filesystem::temp_directory_path(ec) / name;
        std::filesystem::create_directories(dest_, ec);
        int rc = -1;
        if (backend_ == "rar") {
            std::string unrar = find_tool("unrar");
            if (unrar.empty()) unrar = find_tool("bsdtar");
            if (unrar.empty())
                throw UnsupportedFormatError("需要 RAR 后端", "需安装 unrar");
            if (unrar.find("bsdtar") != std::string::npos)
                rc = run_quiet(quote(unrar) + " -x -y -f " + quote(archive.string()) + " -C " + quote(dest_.string()));
            else
                rc = run_quiet(quote(unrar) + " x -y -inul " + quote(archive.string()) + " " + quote(dest_.string() + "\\"));
        } else {
            std::string sevenz = find_tool("7z");
            if (sevenz.empty())
                throw UnsupportedFormatError("需要 7z 后端", "需安装 7-Zip");
            rc = run_quiet(quote(sevenz) + " x -y -o" + quote(dest_.string()) + " " + quote(archive.string()));
        }
        if (rc != 0)
            throw ParseError(backend_ == "rar" ? "RAR 容器无法打开（解压失败）" : "7z 容器无法打开（解压失败）");
        walk(dest_, "");
        for (size_t i = 0; i < entries_.size(); ++i) index_[entries_[i].name] = i;
    }

    ~ExtractedContainer() override {
        std::error_code ec;
        std::filesystem::remove_all(dest_, ec);  // 临时解压目录随容器销毁
    }

    const std::vector<ContainerEntry>& entries() const override { return entries_; }

    std::string read(const std::string& name, long long entry_max_bytes) override {
        std::string key = posix_normpath(name);
        auto it = index_.find(key);
        if (it == index_.end())
            throw AssetNotFoundError(backend_ == "rar" ? "RAR 内不存在条目: " + name
                                                       : "7z 内不存在条目: " + name);
        std::ifstream in(dest_ / std::filesystem::u8path(key), std::ios::binary);
        if (!in) throw ParseError("条目读取失败 " + name);
        if (entry_max_bytes > 0) {
            in.seekg(0, std::ios::end);
            long long sz = (long long)in.tellg();
            if (sz > entry_max_bytes) throw ParseError(limit_error(entry_max_bytes, key));
            in.seekg(0);
        }
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return data;
    }

private:
    void walk(const std::filesystem::path& dir, const std::string& prefix) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) return;
            std::string name = path_u8(it->path().filename());
            std::string rel = prefix.empty() ? name : prefix + "/" + name;
            if (it->is_directory(ec)) { walk(it->path(), rel); continue; }
            if (ec || !it->is_regular_file(ec)) continue;
            auto sz = (unsigned long long)it->file_size(ec);
            entries_.push_back({posix_normpath(rel), sz, sz, 0.0});
        }
    }

    std::string backend_;
    std::filesystem::path dest_;
    std::vector<ContainerEntry> entries_;
    std::map<std::string, size_t> index_;
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
    // 缺失工具的判定放在 ExtractedContainer 前，保证"找不到工具必须抛"的契约
    if (find_tool("unrar").empty() && find_tool("bsdtar").empty())
        throw UnsupportedFormatError("需要 RAR 后端", "需安装 unrar");
    return std::make_unique<ExtractedContainer>("rar", path);
}

std::unique_ptr<Container> open_7z(const std::filesystem::path& path) {
    if (find_tool("7z").empty())
        throw UnsupportedFormatError("需要 7z 后端", "需安装 7-Zip");
    return std::make_unique<ExtractedContainer>("7z", path);
}

// 探测结果缓存：目录列表里每个条目都会问一次 openable，不缓存等于每次列目录
// 都做几百次 SearchPath（Python registry.py:139-155 同样缓存）。
bool rar_backend_available() {
    static const bool ok = [] { return !find_tool("unrar").empty() || !find_tool("bsdtar").empty(); }();
    return ok;
}

bool seven_backend_available() {
    static const bool ok = [] { return !find_tool("7z").empty(); }();
    return ok;
}

}  // namespace lnv
