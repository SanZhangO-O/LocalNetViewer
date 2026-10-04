#pragma once
// 统一错误码与错误类型（对应 Python app/models/schemas.py ErrorCode）
#include <optional>
#include <stdexcept>
#include <string>
#include "nlohmann/json.hpp"

namespace lnv {

enum class ErrorCode {
    BadRequest,
    BadPath,
    PathTraversal,
    Unauthorized,
    BookNotFound,
    LayoutNotFound,
    PageOutOfRange,
    AssetNotFound,
    UnsupportedFormat,
    ParseFailed,
    ServerBusy,
    NotFound,
    Internal,
};

const char* error_code_string(ErrorCode code);
int error_http_status(ErrorCode code);

struct AppError : std::runtime_error {
    ErrorCode code;
    nlohmann::json detail;
    // 显式 HTTP 状态覆盖（0 = 由 code 推导）。对应 errors.py 里
    // AppError(..., http_status=exc.status_code) 的用法：Starlette 的 404/405
    // 处理器把状态原样保留，但错误码被映射成 BAD_PATH / BAD_REQUEST。
    int http_status = 0;
    AppError(ErrorCode c, const std::string& message, nlohmann::json d = nullptr,
             int status = 0)
        : std::runtime_error(message), code(c), detail(std::move(d)), http_status(status) {}
};

// 统一错误响应体 {"code": ..., "message": ..., "detail": ...}
nlohmann::json error_body(ErrorCode code, const std::string& message,
                          const nlohmann::json& detail = nullptr);

}  // namespace lnv
