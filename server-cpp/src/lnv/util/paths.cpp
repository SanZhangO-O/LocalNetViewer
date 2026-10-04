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

namespace {
// UTF-8 窄字符串 → 宽字符 path（operator/=(const char*) 按 ANSI 解释，中文段会被毁）
std::filesystem::path path_from_utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return std::filesystem::path(w);
}
}  // namespace

std::filesystem::path resolve_in_root(const std::filesystem::path& root,
                                      const std::vector<std::string>& rel_parts) {
    std::error_code ec;
    std::filesystem::path real_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) real_root = root;
    if (rel_parts.empty()) return real_root;
    std::filesystem::path candidate = root;
    for (const auto& seg : rel_parts) candidate /= path_from_utf8(seg);
    std::filesystem::path real = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) real = candidate;
    // real 必须等于 real_root 或位于其内
    auto rstr = real.wstring(), rroot = real_root.wstring();
    if (rstr != rroot) {
        if (rstr.size() <= rroot.size() ||
            rstr.compare(0, rroot.size(), rroot) != 0 ||
            (rstr[rroot.size()] != L'\\' && rstr[rroot.size()] != L'/')) {
            throw TraversalError("路径越出书库根目录: " + real.string());
        }
    }
    return real;
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

}  // namespace lnv
