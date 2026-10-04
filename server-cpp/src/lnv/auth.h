#pragma once
// 令牌鉴权（对应 Python api/deps.py require_token）
#include <optional>
#include <string>
#include "lnv/config.h"

namespace lnv {

// 校验 X-Auth-Token；失败抛 AppError(Unauthorized)
void require_token(const AppConfig& cfg, const std::optional<std::string>& header);

}  // namespace lnv
