"""虚拟路径与文件系统路径的安全换算（设计 4.8 / 9）。

虚拟路径形如 ``/我的书库/漫画/第一卷``：第一段是配置里的书库名，
其余段是书库根目录下的相对路径。所有对外暴露的路径都是虚拟路径，
真实路径永不返回给客户端。
"""

from __future__ import annotations

import os
from pathlib import Path, PurePosixPath
from typing import List, Optional
from urllib.parse import unquote


class PathError(Exception):
    """路径非法（BAD_PATH）。"""


class TraversalError(Exception):
    """越出书库根目录（PATH_TRAVERSAL）。"""


_WINDOWS_RESERVED = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}


def normalize_vpath(raw: str, *, allow_empty: bool = False) -> str:
    """URL 解码 + 规范化虚拟路径。

    - 反斜杠统一为 ``/``；
    - 折叠重复分隔符与 ``.`` 段；
    - 拒绝 ``..`` 段、NUL、绝对 Windows 盘符（防目录穿越）。

    **不裁剪段内的空白**：虚拟路径是文件系统名字的镜像，目录 " 漫画" 与 "漫画"
    是两个不同的目录。旧实现用 ``seg.strip()``，导致 ``/lib/ 漫画`` 被规范化成
    ``/lib/漫画`` —— 列表里能看见、点进去却报"路径不存在"；两个只差首尾空格的
    文件也会塌成同一个虚拟路径。整体输入的收尾空白仍然会被去掉。
    """
    if raw is None:
        raise PathError("path 缺失")
    s = unquote(str(raw)).strip()
    if not s:
        if allow_empty:
            return "/"
        raise PathError("path 不能为空")
    if "\x00" in s:
        raise PathError("path 含非法字符")
    s = s.replace("\\", "/")
    if not s.startswith("/"):
        s = "/" + s

    parts: List[str] = []
    for seg in s.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            raise PathError("path 不允许包含 '..'")
        if ":" in seg:
            # Windows 盘符 / NTFS 数据流
            raise PathError("path 段含非法字符 ':'")
        if seg.lower().split(".")[0] in _WINDOWS_RESERVED:
            raise PathError(f"path 段使用了保留名: {seg}")
        parts.append(seg)

    if not parts:
        return "/"
    return "/" + "/".join(parts)


def vpath_parts(vpath: str) -> List[str]:
    v = normalize_vpath(vpath, allow_empty=True)
    return [p for p in v.split("/") if p]


def parent_vpath(vpath: str) -> Optional[str]:
    v = normalize_vpath(vpath, allow_empty=True)
    if v == "/":
        return None
    parent = v.rsplit("/", 1)[0]
    return parent or "/"


def join_vpath(base: str, *names: str) -> str:
    segs = vpath_parts(base)
    for n in names:
        n = str(n).replace("\\", "/").strip("/")
        if n:
            segs.append(n)
    return "/" + "/".join(segs) if segs else "/"


def file_name_of(vpath: str) -> str:
    v = normalize_vpath(vpath, allow_empty=True)
    return v.rsplit("/", 1)[-1] if v != "/" else ""


def ext_of(path_or_name: str) -> str:
    name = path_or_name.replace("\\", "/").rsplit("/", 1)[-1]
    idx = name.rfind(".")
    if idx <= 0:
        return ""
    return name[idx:].lower()


def stem_of(name: str) -> str:
    name = name.replace("\\", "/").rsplit("/", 1)[-1]
    idx = name.rfind(".")
    return name[:idx] if idx > 0 else name


def is_within(child: Path, parent: Path) -> bool:
    """child 是否位于 parent 之内（均已 resolve）。"""
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def resolve_in_root(root: Path, rel_parts: List[str]) -> Path:
    """把书库根 + 相对段解析为真实路径，并校验仍在根内。

    - 显式拒绝符号链接逃逸：解析后的真实路径必须仍位于根目录内；
    - 不要求路径存在（调用方自行判断 404 / BAD_PATH）。
    """
    if not rel_parts:
        real_root = root.resolve()
        return real_root

    candidate = root
    for seg in rel_parts:
        candidate = candidate / seg

    try:
        real_root = root.resolve(strict=False)
        real = candidate.resolve(strict=False)
    except OSError as exc:  # pragma: no cover - 平台相关
        raise PathError(f"路径无法解析: {exc}") from exc
    except ValueError as exc:
        # 例如路径段含 NUL 或非法字符（调用方可能未先过 normalize_vpath）
        raise PathError(f"路径含非法字符: {exc}") from exc

    if real != real_root and not is_within(real, real_root):
        raise TraversalError(f"路径越出书库根目录: {real}")
    return real


def is_hidden_name(name: str) -> bool:
    return name.startswith(".") and name not in (".", "..")


def safe_asset_path(raw: str) -> str:
    """书内资源路径规范化（``/asset?path=`` 用）：相对容器内部路径。"""
    if raw is None:
        raise PathError("asset path 缺失")
    s = unquote(str(raw)).replace("\\", "/").strip()
    if "\x00" in s:
        raise PathError("asset path 含非法字符")
    parts: List[str] = []
    for seg in s.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            raise PathError("asset path 不允许包含 '..'")
        parts.append(seg)
    if not parts:
        raise PathError("asset path 不能为空")
    return "/".join(parts)


def posix_dirname(path: str) -> str:
    return str(PurePosixPath(path).parent).replace("\\", "/")


def normalize_asset_key(path: str) -> str:
    """容器内条目名规范化，用于与解析时记录的 asset 键比对（宽松匹配用）。"""
    s = str(path).replace("\\", "/")
    while s.startswith("./"):
        s = s[2:]
    return s


def makedirs_quiet(path: Path) -> None:
    try:
        os.makedirs(path, exist_ok=True)
    except OSError:
        pass
