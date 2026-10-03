"""FastAPI 依赖：AppContext 注入与可选令牌校验（设计 5.1 / 9）。"""

from __future__ import annotations

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
    if (x_auth_token or "").strip() != expected:
        raise AppError(
            ErrorCode.UNAUTHORIZED,
            "令牌缺失或错误",
            {"auth_required": True},
        )


CtxDep = Depends(get_ctx)
AuthDep = Depends(require_token)
