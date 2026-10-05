"""书库访问范围：按 X-Auth-Token（密码 / 管理员令牌）决定可见书库（设计 9）。

规则（Python 与 C++ 服务器共用同一契约）：

- 每个书库可配置一个 ``password``；留空 = 公开书库（任何人可见）。
- ``server.auth_token`` 是管理员令牌：匹配它可见**全部**书库。
- 匹配某个书库密码的令牌可见"该书库 + 所有公开书库"（密码相同的书库一起可见）。
- 输入了令牌但谁都不匹配 → 401（避免静默只显示公开书库，用户以为密码生效了）。
- 未输入令牌：若存在公开书库则只可见公开书库；若全部书库都加了密码（或启用了
  管理员令牌）→ 401，客户端据此提示输入密码。
"""

from __future__ import annotations

import hmac
from dataclasses import dataclass
from typing import FrozenSet, Optional

from ..config import AppConfig


def _same(provided: str, expected: str) -> bool:
    """常量时间比较（与 api/deps.py 原实现一致；含非 ASCII 时按 UTF-8 字节比较）。"""
    return hmac.compare_digest(provided.encode("utf-8"), expected.encode("utf-8"))


def _normalize_header(value: str) -> str:
    """还原 HTTP 头里被 latin-1 解码的 UTF-8 字节。

    Starlette 按 latin-1 解码请求头：客户端发送的中文密码（UTF-8 字节）会变成
    U+0080..U+00FF 的一串字符，直接与配置里的中文密码比较永远不相等。这里先取回
    原始字节再按 UTF-8 解码；本地就是 latin-1 文本（或解码失败）时保持原样。
    """
    try:
        raw = value.encode("latin-1")
    except UnicodeEncodeError:
        return value
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return value


@dataclass(frozen=True)
class AccessScope:
    """一次请求可见的书库集合。``all=True`` 表示管理员（不受 roots 限制）。"""

    all: bool = False
    roots: FrozenSet[str] = frozenset()

    def allows_root(self, name: Optional[str]) -> bool:
        return bool(name) and (self.all or name in self.roots)


ALL_SCOPE = AccessScope(all=True)
EMPTY_SCOPE = AccessScope()


@dataclass(frozen=True)
class AccessResult:
    """凭据解析结果。

    - ``valid``：凭据可用（含"无需凭据的公开访问"）。
    - ``missing``：未提供凭据但服务器要求（管理员令牌启用 / 全部书库加锁）。
    - ``scope``：可见书库集合（``valid=False`` 时为空集）。
    """

    valid: bool
    missing: bool
    scope: AccessScope


def resolve_access(cfg: AppConfig, token: Optional[str]) -> AccessResult:
    provided = _normalize_header(token or "").strip()
    admin = (cfg.server.auth_token or "").strip()
    public = frozenset(cfg.public_root_names)

    if provided:
        if admin and _same(provided, admin):
            return AccessResult(True, False, ALL_SCOPE)
        matched = frozenset(
            r.name
            for r in cfg.library.roots
            if (r.password or "").strip() and _same(provided, r.password.strip())
        )
        if matched:
            return AccessResult(True, False, AccessScope(False, matched | public))
        return AccessResult(False, False, EMPTY_SCOPE)

    if admin:
        return AccessResult(False, True, EMPTY_SCOPE)
    if cfg.library.roots and not public:
        return AccessResult(False, True, EMPTY_SCOPE)
    return AccessResult(True, False, AccessScope(False, public))
