"""统一错误模型与异常处理器（设计 5.1）。

错误响应体固定为 ``{"code", "message", "detail"}``；错误码 → HTTP 状态映射见
``models.schemas.ERROR_HTTP_STATUS``。
"""

from __future__ import annotations

import logging
from typing import Any, Dict, Optional

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from ..formats.base import (
    AssetNotFoundError,
    ParseError,
    ParseTimeoutError,
    UnsupportedFormatError,
)
from ..models.schemas import ERROR_HTTP_STATUS, ErrorCode
from ..util.imaging import ImageDecodeError, ImageTooLargeError
from ..util.paths import PathError, TraversalError

logger = logging.getLogger("localnetviewer.api")


class AppError(Exception):
    """业务错误：携带错误码、HTTP 状态与 detail。"""

    def __init__(
        self,
        code: ErrorCode | str,
        message: str,
        detail: Optional[Dict[str, Any]] = None,
        http_status: Optional[int] = None,
    ) -> None:
        super().__init__(message)
        self.code = code.value if isinstance(code, ErrorCode) else str(code)
        self.message = message
        self.detail = detail or {}
        self.http_status = int(
            http_status or ERROR_HTTP_STATUS.get(self.code, 500)
        )

    def payload(self) -> Dict[str, Any]:
        body: Dict[str, Any] = {"code": self.code, "message": self.message}
        if self.detail:
            body["detail"] = self.detail
        return body


def error_response(err: AppError) -> JSONResponse:
    headers = {}
    if err.code == ErrorCode.UNAUTHORIZED.value:
        headers["WWW-Authenticate"] = "X-Auth-Token"
    return JSONResponse(status_code=err.http_status, content=err.payload(), headers=headers)


def install_exception_handlers(app: FastAPI) -> None:
    @app.exception_handler(AppError)
    async def _app_error(_: Request, exc: AppError) -> JSONResponse:
        return error_response(exc)

    @app.exception_handler(UnsupportedFormatError)
    async def _unsupported(_: Request, exc: UnsupportedFormatError) -> JSONResponse:
        detail = {"reason": exc.reason} if getattr(exc, "reason", None) else {}
        return error_response(
            AppError(ErrorCode.UNSUPPORTED_FORMAT, str(exc) or "格式不支持", detail)
        )

    @app.exception_handler(ParseTimeoutError)
    async def _timeout(_: Request, exc: ParseTimeoutError) -> JSONResponse:
        return error_response(
            AppError(ErrorCode.PARSE_FAILED, str(exc) or "解析超时", {"reason": "timeout"})
        )

    @app.exception_handler(ParseError)
    async def _parse(_: Request, exc: ParseError) -> JSONResponse:
        return error_response(AppError(ErrorCode.PARSE_FAILED, str(exc) or "解析失败"))

    # 图片解码类异常是 RuntimeError 的子类，不注册就会落到通用处理器变成 500。
    # 它们在 comics.pair_bytes 等路径上会直接冒出来（同一条路径的 page_bytes
    # 已经把同样的错误转成 ParseError），契约上应当是 422 PARSE_FAILED。
    @app.exception_handler(ImageTooLargeError)
    async def _image_too_large(_: Request, exc: ImageTooLargeError) -> JSONResponse:
        return error_response(
            AppError(ErrorCode.PARSE_FAILED, str(exc) or "图片超出像素上限", {"reason": "image_too_large"})
        )

    @app.exception_handler(ImageDecodeError)
    async def _image_decode(_: Request, exc: ImageDecodeError) -> JSONResponse:
        return error_response(
            AppError(ErrorCode.PARSE_FAILED, str(exc) or "图片解码失败", {"reason": "image_decode"})
        )

    @app.exception_handler(TraversalError)
    async def _traversal(_: Request, exc: TraversalError) -> JSONResponse:
        return error_response(AppError(ErrorCode.PATH_TRAVERSAL, str(exc) or "越出书库根目录"))

    @app.exception_handler(PathError)
    async def _bad_path(_: Request, exc: PathError) -> JSONResponse:
        return error_response(AppError(ErrorCode.BAD_PATH, str(exc) or "路径非法"))

    @app.exception_handler(AssetNotFoundError)
    async def _asset(_: Request, exc: AssetNotFoundError) -> JSONResponse:
        return error_response(AppError(ErrorCode.ASSET_NOT_FOUND, str(exc) or "资源不存在"))

    @app.exception_handler(RequestValidationError)
    async def _validation(_: Request, exc: RequestValidationError) -> JSONResponse:
        return error_response(
            AppError(
                ErrorCode.BAD_REQUEST,
                "请求参数缺失或非法",
                {"errors": _safe_errors(exc.errors())},
            )
        )

    @app.exception_handler(StarletteHTTPException)
    async def _http(_: Request, exc: StarletteHTTPException) -> JSONResponse:
        code = ErrorCode.BAD_REQUEST
        if exc.status_code == 404:
            code = ErrorCode.BAD_PATH
        elif exc.status_code == 401:
            code = ErrorCode.UNAUTHORIZED
        elif exc.status_code == 403:
            code = ErrorCode.PATH_TRAVERSAL
        elif exc.status_code >= 500:
            code = ErrorCode.INTERNAL
        return error_response(
            AppError(code, str(exc.detail) or "请求失败", http_status=exc.status_code)
        )

    @app.exception_handler(Exception)
    async def _unhandled(_: Request, exc: Exception) -> JSONResponse:
        logger.exception("未处理的服务器异常: %s", exc)
        return error_response(
            AppError(ErrorCode.INTERNAL, "服务器内部错误", {"type": type(exc).__name__})
        )


def _safe_errors(errors: Any) -> list:
    out = []
    for item in errors or []:
        try:
            out.append(
                {
                    "loc": [str(x) for x in item.get("loc", [])],
                    "msg": str(item.get("msg", "")),
                    "type": str(item.get("type", "")),
                }
            )
        except Exception:
            continue
    return out[:20]
