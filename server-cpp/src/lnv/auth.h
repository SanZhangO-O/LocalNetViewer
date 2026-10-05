#pragma once
// 令牌 / 书库密码鉴权（对应 Python services/access.py + api/deps.py，设计 9）
//
// 规则：
// - X-Auth-Token 匹配 server.auth_token（管理员令牌）→ 全部书库；
// - 匹配某个 library.roots[].password → 该书库 + 所有公开（未设密码）书库；
// - 未带凭据 → 只有公开书库；若管理员令牌启用或全部书库加锁 → 401"需要访问密码"；
// - 输入了凭据但谁都不匹配 → 401"访问密码错误"（不静默降级）。
#include <optional>
#include <set>
#include <string>
#include "lnv/config.h"

namespace lnv {

struct AccessScope {
    bool all = false;
    std::set<std::string> roots;

    bool allows(const std::string& name) const {
        return all || (roots.find(name) != roots.end());
    }
};

struct AccessResult {
    bool valid = false;
    bool missing = false;
    AccessScope scope;
};

// 解析凭据（不抛异常；调用方决定 missing/invalid 的响应）
AccessResult resolve_access(const AppConfig& cfg, const std::optional<std::string>& header);

// 校验 X-Auth-Token；失败抛 AppError(Unauthorized)（与 Python deps.access_scope 同文案）
void require_token(const AppConfig& cfg, const std::optional<std::string>& header);

}  // namespace lnv
