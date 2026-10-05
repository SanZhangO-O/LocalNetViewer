#include "lnv/util/paths.h"
#include <algorithm>
#include <cctype>
#include <windows.h>

namespace lnv {

namespace {
const char* const kWindowsReserved[] = {"con","prn","aux","nul","com1","com2","com3",
    "com4","com5","com6","com7","com8","com9","lpt1","lpt2","lpt3","lpt4","lpt5",
    "lpt6","lpt7","lpt8","lpt9"};

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

bool is_safe_id(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string url_unquote(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex_val(s[i + 1]), l = hex_val(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(char((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string normalize_vpath(const std::string& raw, bool allow_empty) {
    std::string s = url_unquote(raw);
    // 去掉首尾空白（字节级，与 Python str.strip 等价足够）
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    if (s.empty()) {
        if (allow_empty) return "/";
        throw PathError("path 不能为空");
    }
    if (s.find('\0') != std::string::npos) throw PathError("path 含非法字符");
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.front() != '/') s.insert(s.begin(), '/');

    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string seg = s.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") throw PathError("path 不允许包含 '..'");
        if (seg.find(':') != std::string::npos)
            throw PathError("path 段含非法字符 ':'");
        std::string low = to_lower(seg);
        size_t dot = low.find('.');
        std::string head = dot == std::string::npos ? low : low.substr(0, dot);
        for (const char* r : kWindowsReserved)
            if (head == r) throw PathError("path 段使用了保留名: " + seg);
        parts.push_back(std::move(seg));
    }
    if (parts.empty()) return "/";
    std::string out;
    for (auto& p : parts) { out.push_back('/'); out += p; }
    return out;
}

std::vector<std::string> vpath_parts(const std::string& vpath) {
    std::string v = normalize_vpath(vpath, true);
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find('/', i);
        if (j == std::string::npos) j = v.size();
        if (j > i) parts.push_back(v.substr(i, j - i));
        i = j + 1;
    }
    return parts;
}

std::optional<std::string> parent_vpath(const std::string& vpath) {
    std::string v = normalize_vpath(vpath, true);
    if (v == "/") return std::nullopt;
    size_t pos = v.rfind('/');
    std::string parent = v.substr(0, pos);
    return parent.empty() ? std::optional<std::string>{"/"} : std::optional<std::string>{parent};
}

std::string join_vpath(const std::string& base, const std::vector<std::string>& names) {
    auto segs = vpath_parts(base);
    for (const auto& n0 : names) {
        std::string n = n0;
        std::replace(n.begin(), n.end(), '\\', '/');
        // strip '/'
        size_t b = n.find_first_not_of('/');
        size_t e = n.find_last_not_of('/');
        if (b == std::string::npos) continue;
        segs.push_back(n.substr(b, e - b + 1));
    }
    if (segs.empty()) return "/";
    std::string out;
    for (auto& s : segs) { out.push_back('/'); out += s; }
    return out;
}

std::string file_name_of(const std::string& vpath) {
    std::string v = normalize_vpath(vpath, true);
    if (v == "/") return "";
    return v.substr(v.rfind('/') + 1);
}

std::string ext_of(const std::string& path_or_name) {
    std::string name = path_or_name;
    std::replace(name.begin(), name.end(), '\\', '/');
    size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return to_lower(name.substr(dot));
}

std::string stem_of(const std::string& name) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    size_t slash = n.rfind('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    size_t dot = n.rfind('.');
    return (dot == std::string::npos || dot == 0) ? n : n.substr(0, dot);
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto& w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string s((size_t)n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
    if (w.empty()) return {};
    // WideCharToMultiByte 失败（极端输入）：退回 C++ 标准转换
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

namespace {
// UTF-8 窄字符串 → 宽字符 path（operator/=(const char*) 按 ANSI 解释，中文段会被毁）
std::filesystem::path path_from_utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return std::filesystem::path(w);
}

// \\?\ 扩展长度形式（>MAX_PATH 时普通 Win32 路径会失败）：absolute + 词法规范化
// 后加前缀；UNC 用 \\?\UNC\ 形式。失败返回 nullopt。
std::optional<std::filesystem::path> extended_length_path(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) return std::nullopt;
    std::wstring w = abs.lexically_normal().native();
    for (auto& ch : w) {
        if (ch == L'/') ch = L'\\';  // \\?\ 前缀下 Win32 不再换算 '/'，统一反斜杠
    }
    if (w.rfind(L"\\\\?\\", 0) == 0) return std::nullopt;  // 已是扩展形式
    if (w.rfind(L"\\\\", 0) == 0)
        return std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
    if (w.size() >= 2 && w[1] == L':') return std::filesystem::path(L"\\\\?\\" + w);
    return std::nullopt;
}

// canonical 解析（weakly：不要求存在，与 Python Path.resolve(strict=False) 同语义）。
// 失败不退回纯词法比较：那会在两侧都失败时完全不解析符号链接，逃逸检测失效。
std::filesystem::path canonical_or_fail(const std::filesystem::path& p) {
    std::error_code ec;
    auto real = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return real;
    if (auto ext = extended_length_path(p)) {
        std::error_code ec2;
        auto real2 = std::filesystem::weakly_canonical(*ext, ec2);
        if (!ec2) {
            // MSVC 可能把 \\?\ 前缀去掉；长路径必须保前缀，后续文件操作才可用
            std::wstring w = real2.native();
            if (w.rfind(L"\\\\?\\", 0) != 0) {
                for (auto& ch : w) {
                    if (ch == L'/') ch = L'\\';
                }
                if (w.rfind(L"\\\\", 0) == 0)
                    real2 = std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
                else
                    real2 = std::filesystem::path(L"\\\\?\\" + w);
            }
            return real2;
        }
    }
    throw PathError("路径无法解析: " + path_to_utf8(p));
}

// 比较用：剥掉 \\?\ / \\?\UNC\ 前缀（扩展与普通形式指向同一位置）
std::wstring comparable_path(const std::filesystem::path& p) {
    std::wstring w = p.native();
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + w.substr(8);
    if (w.rfind(L"\\\\?\\", 0) == 0) return w.substr(4);
    return w;
}
}  // namespace

std::filesystem::path resolve_in_root(const std::filesystem::path& root,
                                      const std::vector<std::string>& rel_parts) {
    std::filesystem::path real_root = canonical_or_fail(root);
    if (rel_parts.empty()) return real_root;
    std::filesystem::path candidate = real_root;
    for (const auto& seg : rel_parts) candidate /= path_from_utf8(seg);
    std::filesystem::path real = canonical_or_fail(candidate);
    // real 必须等于 real_root 或位于其内（用统一去掉扩展前缀后的形式做分隔符边界比较）
    auto rstr = comparable_path(real), rroot = comparable_path(real_root);
    if (rstr != rroot) {
        if (rstr.size() <= rroot.size() ||
            rstr.compare(0, rroot.size(), rroot) != 0 ||
            (rstr[rroot.size()] != L'\\' && rstr[rroot.size()] != L'/')) {
            throw TraversalError("路径越出书库根目录: " + path_to_utf8(real));
        }
    }
    return real;
}

bool is_symlink_or_junction(const std::filesystem::path& p) {
    // FindFirstFileW 一次调用同时给出属性与重解析标签（dwReserved0），比打开句柄便宜。
    // 只认 symlink / MOUNT_POINT：Python 的 islink 只认 symlink（junction 被当目录，
    // os.walk 会跟进成环，bpo-23407），这里多跳 junction 是防环加固。
    // OneDrive 等云占位符也是 reparse point 但不是链接，Python 不跳过，这里也不能跳过。
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(p.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // 长路径下普通查询会失败，换 \\?\ 形式重试（对应 resolve_in_root 的长路径支持）
        if (auto ext = extended_length_path(p)) h = FindFirstFileW(ext->c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
    }
    FindClose(h);
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
    return fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
           fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}

bool is_hidden_name(const std::string& name) {
    return name.size() > 1 && name[0] == '.';
}

std::string safe_asset_path(const std::string& raw) {
    if (raw.empty()) throw PathError("asset path 缺失");
    std::string s = url_unquote(raw);
    std::replace(s.begin(), s.end(), '\\', '/');
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    if (s.find('\0') != std::string::npos) throw PathError("asset path 含非法字符");
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string seg = s.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") throw PathError("asset path 不允许包含 '..'");
        parts.push_back(std::move(seg));
    }
    if (parts.empty()) throw PathError("asset path 不能为空");
    std::string out;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out.push_back('/');
        out += parts[k];
    }
    return out;
}

std::string normalize_asset_key(const std::string& path) {
    std::string s = path;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.rfind("./", 0) == 0) s = s.substr(2);
    return s;
}

std::wstring find_tool_in_path(const std::wstring& base_name) {
    DWORD need = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (need == 0) return {};
    std::wstring path(need, L'\0');
    DWORD got = GetEnvironmentVariableW(L"PATH", path.data(), need);
    if (got == 0 || got >= need) return {};
    path.resize(got);
    const std::wstring exe = base_name + L".exe";
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(L';', i);
        if (j == std::wstring::npos) j = path.size();
        std::wstring dir = path.substr(i, j - i);
        while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'\t')) dir.erase(dir.begin());
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'\t')) dir.pop_back();
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"')
            dir = dir.substr(1, dir.size() - 2);  // PATH 条目允许被引号包裹
        if (!dir.empty()) {
            std::error_code ec;
            auto cand = std::filesystem::path(dir) / exe;
            if (std::filesystem::is_regular_file(cand, ec)) return cand.wstring();
        }
        if (j >= path.size()) break;
        i = j + 1;
    }
    return {};
}

std::wstring quote_windows_arg(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++bs; out.push_back(c); continue; }
        if (c == L'"') {
            out.append(bs + 1, L'\\');
            out.push_back(L'"');
            bs = 0;
            continue;
        }
        bs = 0;
        out.push_back(c);
    }
    out.append(bs, L'\\');
    out.push_back(L'"');
    return out;
}

}  // namespace lnv
