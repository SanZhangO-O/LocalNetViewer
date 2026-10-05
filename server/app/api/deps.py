"""FastAPI 依赖：AppContext 注入与访问范围解析（设计 5.1 / 9）。

``X-Auth-Token`` 既可以是 ``server.auth_token``（管理员令牌，见全部书库），
也可以是某个书库的 ``password``（只见该书库 + 公开书库）。规则见 services/access.py。
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from fastapi import Depends, Header, Request

from ..models.schemas import ErrorCode
from ..services.access import AccessScope, resolve_access
from .errors import AppError

if TYPE_CHECKING:  # pragma: no cover
    from ..context import AppContext


def get_ctx(request: Request) -> "AppContext":
    ctx = getattr(request.app.state, "ctx", None)
    if ctx is None:  # pragma: no cover - 装配错误
        raise AppError(ErrorCode.INTERNAL, "服务器上下文未初始化")
    return ctx


def access_scope(
    request: Request,
    x_auth_token: str | None = Header(default=None, alias="X-Auth-Token"),
) -> AccessScope:
    """解析请求可见的书库范围；凭据无效 / 缺失（且必须提供）→ 401。"""
    ctx = get_ctx(request)
    result = resolve_access(ctx.config, x_auth_token)
    if not result.valid:
        raise AppError(
            ErrorCode.UNAUTHORIZED,
            "需要访问密码" if result.missing else "访问密码错误",
            {"auth_required": True},
        )
    return result.scope


def require_token(
    request: Request,
    x_auth_token: str | None = Header(default=None, alias="X-Auth-Token"),
) -> None:
    """仅校验凭据（不需要按书库过滤的端点用，如字体列表）。"""
    access_scope(request, x_auth_token)


CtxDep = Depends(get_ctx)
ScopeDep = Depends(access_scope)
AuthDep = Depends(require_token)
