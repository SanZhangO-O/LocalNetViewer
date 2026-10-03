"""FastAPI 依赖：AppContext 注入与可选令牌校验（设计 5.1 / 9）。"""

from __future__ import annotations

import secrets
from typing import TYPE_CHECKING

from fastapi import Depends, Header, Request

from ..models.schemas import ErrorCode
from .errors import AppError

if TYPE_CHECKING:  # pragma: no cover
    from ..context import AppContext


def get_ctx(request: Request) -> "AppContext":
    ctx = getattr(request.app.state, "ctx", None)
    if ctx is None:  # pragma: no cover - 装配错误
        raise AppError(ErrorCode.INTERNAL, "服务器上下文未初始化")
    return ctx


def require_token(
    request: Request,
    x_auth_token: str | None = Header(default=None, alias="X-Auth-Token"),
) -> None:
    """配置了 ``server.auth_token`` 时校验令牌；``/server/info`` 豁免。"""
    ctx = get_ctx(request)
    expected = (ctx.config.server.auth_token or "").strip()
    if not expected:
        return
    # 常量时间比较，避免逐字节短路造成的时序侧信道。
    # encode 成 bytes：compare_digest 对含非 ASCII 字符的 str 会抛 TypeError（500），
    # 而这里应当返回 401。
    if not secrets.compare_digest(
        (x_auth_token or "").strip().encode("utf-8"),
        expected.encode("utf-8"),
    ):
        raise AppError(
            ErrorCode.UNAUTHORIZED,
            "令牌缺失或错误",
            {"auth_required": True},
        )


CtxDep = Depends(get_ctx)
AuthDep = Depends(require_token)
