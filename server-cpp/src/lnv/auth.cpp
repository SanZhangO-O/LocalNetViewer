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
}  // namespace

void require_token(const AppConfig& cfg, const std::optional<std::string>& header) {
    if (!cfg.auth_required()) return;
    std::string expect = strip_ws(cfg.server.auth_token);
    std::string got = header ? strip_ws(*header) : "";
    // 常量时间比较（长度不同也循环 max 次）
    size_t n = std::max(expect.size(), got.size());
    unsigned char diff = expect.size() == got.size() ? 0 : 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = i < expect.size() ? (unsigned char)expect[i] : 0;
        unsigned char b = i < got.size() ? (unsigned char)got[i] : 0;
        diff |= a ^ b;
    }
    if (diff) {
        throw AppError(ErrorCode::Unauthorized, "令牌缺失或错误",
                       nlohmann::json{{"auth_required", true}});
    }
}

}  // namespace lnv
