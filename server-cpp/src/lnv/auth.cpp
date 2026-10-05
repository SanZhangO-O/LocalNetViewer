#include "lnv/auth.h"
#include "lnv/types.h"
#include <cstring>

namespace lnv {

namespace {

std::string strip_ws(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// 常量时间比较（长度不同也循环 max 次），与旧 require_token 的实现一致
bool same_secret(const std::string& expect, const std::string& got) {
    size_t n = std::max(expect.size(), got.size());
    unsigned char diff = expect.size() == got.size() ? 0 : 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = i < expect.size() ? (unsigned char)expect[i] : 0;
        unsigned char b = i < got.size() ? (unsigned char)got[i] : 0;
        diff |= a ^ b;
    }
    return diff == 0;
}

}  // namespace

AccessResult resolve_access(const AppConfig& cfg, const std::optional<std::string>& header) {
    AccessResult result;
    const std::string provided = header ? strip_ws(*header) : "";
    const std::string admin = strip_ws(cfg.server.auth_token);

    std::set<std::string> public_roots;
    for (const auto& r : cfg.library.roots)
        if (strip_ws(r.password).empty()) public_roots.insert(r.name);

    if (!provided.empty()) {
        if (!admin.empty() && same_secret(admin, provided)) {
            result.valid = true;
            result.scope.all = true;
            return result;
        }
        std::set<std::string> matched;
        for (const auto& r : cfg.library.roots) {
            const std::string pw = strip_ws(r.password);
            if (!pw.empty() && same_secret(pw, provided)) matched.insert(r.name);
        }
        if (!matched.empty()) {
            result.valid = true;
            result.scope.roots = std::move(matched);
            result.scope.roots.insert(public_roots.begin(), public_roots.end());
            return result;
        }
        return result;  // 有凭据但谁都不匹配 → invalid
    }

    if (!admin.empty()) {
        result.missing = true;
        return result;
    }
    if (!cfg.library.roots.empty() && public_roots.empty()) {
        result.missing = true;  // 全部书库加锁
        return result;
    }
    result.valid = true;
    result.scope.roots = std::move(public_roots);
    return result;
}

void require_token(const AppConfig& cfg, const std::optional<std::string>& header) {
    const AccessResult result = resolve_access(cfg, header);
    if (!result.valid) {
        throw AppError(ErrorCode::Unauthorized,
                       result.missing ? "需要访问密码" : "访问密码错误",
                       nlohmann::json{{"auth_required", true}});
    }
}

}  // namespace lnv
