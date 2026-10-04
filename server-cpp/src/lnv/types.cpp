#include "lnv/types.h"

namespace lnv {

const char* error_code_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest: return "BAD_REQUEST";
        case ErrorCode::BadPath: return "BAD_PATH";
        case ErrorCode::PathTraversal: return "PATH_TRAVERSAL";
        case ErrorCode::Unauthorized: return "UNAUTHORIZED";
        case ErrorCode::BookNotFound: return "BOOK_NOT_FOUND";
        case ErrorCode::LayoutNotFound: return "LAYOUT_NOT_FOUND";
        case ErrorCode::PageOutOfRange: return "PAGE_OUT_OF_RANGE";
        case ErrorCode::AssetNotFound: return "ASSET_NOT_FOUND";
        case ErrorCode::UnsupportedFormat: return "UNSUPPORTED_FORMAT";
        case ErrorCode::ParseFailed: return "PARSE_FAILED";
        case ErrorCode::ServerBusy: return "SERVER_BUSY";
        case ErrorCode::NotFound: return "NOT_FOUND";
        case ErrorCode::Internal: return "INTERNAL";
    }
    return "INTERNAL";
}

int error_http_status(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest: return 400;
        case ErrorCode::BadPath: return 400;
        case ErrorCode::PathTraversal: return 403;
        case ErrorCode::Unauthorized: return 401;
        case ErrorCode::BookNotFound: return 404;
        case ErrorCode::LayoutNotFound: return 404;
        case ErrorCode::PageOutOfRange: return 404;
        case ErrorCode::AssetNotFound: return 404;
        case ErrorCode::NotFound: return 404;
        case ErrorCode::UnsupportedFormat: return 415;
        case ErrorCode::ParseFailed: return 422;
        case ErrorCode::ServerBusy: return 503;
        case ErrorCode::Internal: return 500;
    }
    return 500;
}

nlohmann::json error_body(ErrorCode code, const std::string& message,
                          const nlohmann::json& detail) {
    nlohmann::json j;
    j["code"] = error_code_string(code);
    j["message"] = message;
    // Python errors.py::AppError.payload：detail 非空时才附加 "detail" 键。
    // 恒发 "detail": null 会让客户端/测试看到多出来的键（PORT_SPEC §1.1）。
    if (!detail.is_null() && !(detail.is_object() && detail.empty()))
        j["detail"] = detail;
    return j;
}

}  // namespace lnv
