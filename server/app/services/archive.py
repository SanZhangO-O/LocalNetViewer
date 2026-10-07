"""压缩包内容浏览与包内嵌套书（设计 7.6）。

- **contents 列表**：``GET /books/{id}/contents?entry=<包内目录>`` 把压缩包当
  虚拟目录浏览；条目分类 / 置灰规则与 ``/files`` 一致（图片 / 可打开的书 /
  嵌套压缩包 / 其它文件）；
- **嵌套书**：包内的书、图片、目录可以打开为独立的 book_id（确定性推导，
  见 ``formats.archivebook.nested_book_id``），进度、布局缓存、封面全部复用
  现有书籍链路；注册表持久化在 ``cache/archives.json``（book_id 是哈希、无法
  反推条目路径，重启后必须能解析）；
- **可见性**：嵌套书解析先按链顶找到顶层真实压缩包并走
  ``library.book_by_id(root, scope)`` —— 密码书库"不可见 = 不存在"的防探测
  语义自动继承；
- **失效**：嵌套书修订号绑定顶层压缩包的 revision，压缩包内容变化后旧 id 的
  全部派生缓存自然失效；解压缓存按 LRU 上限淘汰（启动 sweep）。
"""

from __future__ import annotations

import json
import os
import posixpath
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from ..formats import registry
from ..formats.archivebook import (
    nested_book_id,
    nested_revision,
    open_chain_container,
)
from ..formats.base import AssetNotFoundError, BookRef, UnsupportedFormatError
from ..formats.comics import ComicContainer, is_archive_name, is_image_name, open_container
from ..util.paths import PathError, makedirs_quiet, stem_of
from .access import AccessScope

if TYPE_CHECKING:  # pragma: no cover
    from .library import LibraryService

ARCHIVE_PREFIX = "arc:"
MAX_REGISTRY = 5000
CONTENT_LIMIT_MAX = 2000


def normalize_entry(raw: str) -> str:
    """包内条目路径规范化：相对路径、拒绝 ``..`` 与空段（与容器内规范化一致）。"""
    s = str(raw or "").replace("\\", "/").strip().strip("/")
    if not s or "\x00" in s:
        raise PathError("entry 非法")
    parts: List[str] = []
    for seg in s.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            raise PathError("entry 不允许包含 '..'")
        parts.append(seg)
    if not parts:
        raise PathError("entry 不能为空")
    return "/".join(parts)


class ArchiveService:
    def __init__(self, config, library: "LibraryService", env) -> None:
        self.config = config
        self.library = library
        self.env = env
        self.path = config.archive_registry_path
        self._lock = threading.RLock()
        self._save_lock = threading.Lock()
        self._registry: Dict[str, Dict[str, Any]] = {}
        self.load_registry()

    # ------------------------------------------------------------ 注册表
    def load_registry(self) -> None:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            self._registry = {}
            return
        except (OSError, json.JSONDecodeError, ValueError):
            self._registry = {}
            return
        if isinstance(data, dict):
            self._registry = {
                str(k): dict(v) for k, v in data.items() if isinstance(v, dict)
            }

    def save_registry(self) -> None:
        with self._save_lock:
            with self._lock:
                payload = json.dumps(self._registry, ensure_ascii=False, indent=1)
            tmp = self.path.with_suffix(f".tmp{os.getpid()}.{time.time_ns()}")
            try:
                makedirs_quiet(self.path.parent)
                tmp.write_text(payload, encoding="utf-8")
                os.replace(tmp, self.path)
            except OSError:
                try:
                    tmp.unlink(missing_ok=True)
                except OSError:
                    pass

    def registry_size(self) -> int:
        return len(self._registry)

    def _prune_locked(self) -> None:
        overflow = len(self._registry) - MAX_REGISTRY
        if overflow <= 0:
            return
        ordered = sorted(
            self._registry.items(),
            key=lambda item: float(item[1].get("updated_at", 0.0)),
        )
        for key, _ in ordered[:overflow]:
            self._registry.pop(key, None)

    # ------------------------------------------------------------ 容器打开
    def _container_for(self, ref: BookRef) -> ComicContainer:
        """book_id → 最内层压缩包容器（顶层真实包，或嵌套包解压后打开）。"""
        extra = ref.extra or {}
        kind = str(extra.get("kind") or "")
        if kind in ("dir", "image"):
            # 前缀图集/单图嵌套书没有自己的容器（浏览它们没有意义）
            raise UnsupportedFormatError(
                "该条目不是压缩包，无法浏览内容", reason="非压缩包条目"
            )
        chain = [str(x) for x in (extra.get("chain") or [])]
        chain_ids = [str(x) for x in (extra.get("chain_ids") or [])]
        if ref.virtual and chain:
            root_path = Path(str(extra.get("root_path") or ref.path))
            return open_chain_container(self.env, root_path, chain, chain_ids)
        # 非虚拟或未知扩展名：直接尝试打开容器（open_container 内含魔数兜底，
        # 嗅探出来的错扩展名 zip/rar/7z 也能浏览内容）
        container = open_container(self.env, ref.path)
        if not getattr(container, "browsable", False):
            container.close()
            raise UnsupportedFormatError(
                "该书不是可浏览的压缩包", reason="非压缩包格式"
            )
        return container

    # ------------------------------------------------------------ 解析
    def resolve(self, nested_id: str, allowed: Optional[AccessScope] = None) -> Optional[BookRef]:
        """嵌套书 id → BookRef（链顶可见性校验；不可见/不存在返回 None）。"""
        item = self.entry(nested_id)
        if not item:
            return None
        chain: List[Dict[str, Any]] = [item]
        ids: List[str] = [str(nested_id)]
        max_depth = max(1, int(self.config.archive.max_depth))
        while True:
            parent_id = str(chain[-1].get("parent") or "")
            pitem = self.entry(parent_id)
            if pitem is None:
                break
            chain.append(pitem)
            ids.append(parent_id)
            if len(chain) > max_depth:
                return None
        root_id = str(chain[-1].get("parent") or "")
        root_ref = self.library.book_by_id(root_id, allowed)
        if root_ref is None:
            return None
        # 与 /contents 同一能力判定（formats 注册表）：嗅探/覆盖出来的容器
        # 也能解析其嵌套书，不得另用 is_archive_name 扩展名清单做门禁
        root_fmt = registry.format_for_ref(root_ref)
        if not registry.is_browsable_format(root_fmt):
            return None
        entries = [str(c.get("entry") or "") for c in reversed(chain)]
        chain_ids = list(reversed(ids))
        kind = str(item.get("kind") or "book")
        revision = nested_revision(str(nested_id), root_ref.revision)
        vpath = "/".join([root_ref.vpath] + entries)
        return BookRef(
            book_id=str(nested_id),
            vpath=vpath,
            path=root_ref.path,
            format=str(item.get("format") or ""),
            size=0,
            mtime=0.0,
            virtual=True,
            revision=revision,
            extra={
                "archive_entry": entries[-1],
                "chain": entries,
                "chain_ids": chain_ids,
                "root_path": str(root_ref.path),
                "kind": kind,
                "depth": len(entries),
                "title": str(item.get("title") or ""),
            },
        )

    def entry(self, book_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            item = self._registry.get(str(book_id or ""))
            return dict(item) if item else None

    # ------------------------------------------------------------ contents 列表
    def list_contents(
        self,
        ref: BookRef,
        entry_dir: str,
        limit: int = 500,
        offset: int = 0,
    ) -> Dict[str, Any]:
        """压缩包内部目录列表（两阶段分页与 /files 同风格）。"""
        if not bool(self.config.archive.browse):
            raise UnsupportedFormatError(
                "服务器已关闭压缩包内容浏览", reason="archive.browse=false"
            )
        d = normalize_entry(entry_dir) if str(entry_dir or "").strip("/") else ""
        container = self._container_for(ref)
        try:
            entries = container.browse()
        finally:
            try:
                container.close()
            except Exception:
                pass

        pref = d + "/" if d else ""
        if d and not any(e.name == d for e in entries):
            raise AssetNotFoundError(f"压缩包内不存在目录: {d}")
        sel = [e for e in entries if e.name.startswith(pref)] if d else entries

        rows: List[Dict[str, Any]] = []
        for e in sel:
            rel = e.name[len(pref) :] if d else e.name
            if not rel or "/" in rel:
                continue  # 只列直接子级
            if e.is_dir:
                image_count = sum(
                    1
                    for x in entries
                    if not x.is_dir and x.name.startswith(e.name + "/") and is_image_name(x.name)
                )
                min_images = max(1, int(self.config.gallery.min_images_in_folder))
                can_open = image_count >= min_images
                rows.append(
                    {
                        "name": rel,
                        "type": "dir",
                        "path": e.name,
                        "size": None,
                        "gallery": {
                            "image_count": image_count,
                            "can_open_as_book": can_open,
                        },
                        "preview_url": None,
                        "openable": True,
                        "reason": None if can_open else f"图片数不足（{image_count} < {min_images}）",
                    }
                )
                continue
            base = posixpath.basename(e.name)
            if is_image_name(base):
                rows.append(
                    {
                        "name": rel,
                        "type": "image",
                        "path": e.name,
                        "format": "gallery",
                        "size": int(e.size or 0),
                        "gallery": {"image_count": 1, "can_open_as_book": True},
                        "preview_url": _asset_url(ref, e.name),
                        "openable": True,
                        "reason": None,
                    }
                )
                continue
            fmt = registry.format_of(base)
            if fmt:
                reason = registry.openable_reason(fmt)
                rows.append(
                    {
                        "name": rel,
                        "type": "book",
                        "path": e.name,
                        "format": fmt,
                        "size": int(e.size or 0),
                        "gallery": None,
                        "preview_url": None,
                        "openable": reason is None,
                        "reason": reason,
                    }
                )
                continue
            rows.append(
                {
                    "name": rel,
                    "type": "file",
                    "path": e.name,
                    "format": None,
                    "size": int(e.size or 0),
                    "gallery": None,
                    "preview_url": None,
                    "openable": False,
                    "reason": "非电子书格式",
                }
            )

        total = len(rows)
        start = max(0, int(offset))
        page = rows[start : start + max(1, min(int(limit), CONTENT_LIMIT_MAX))]
        files = [e for e in entries if not e.is_dir]
        summary = {
            "entry_count": len(files),
            "image_count": sum(1 for e in files if is_image_name(e.name)),
            "depth": len([str(x) for x in (ref.extra or {}).get("chain") or []]),
            "max_depth": max(1, int(self.config.archive.max_depth)),
        }
        # 子目录的 parent 是包内父目录（"" = 包根）；包根的 parent 为 None
        parent = posixpath.dirname(d) if d else None
        return {
            "book_id": ref.book_id,
            "revision": ref.revision,
            "entry": d,
            "parent": parent,
            "archive": summary,
            "entries": page,
            "total": total,
            "limit": int(limit),
            "offset": int(offset),
        }

    # ------------------------------------------------------------ 打开包内条目
    def open_nested(self, ref: BookRef, entry: str) -> Dict[str, Any]:
        """包内条目 → 嵌套书（幂等注册），返回 OpenArchiveResponse 数据。"""
        if not bool(self.config.archive.browse):
            raise UnsupportedFormatError(
                "服务器已关闭压缩包内容浏览", reason="archive.browse=false"
            )
        e = normalize_entry(entry)
        parent_depth = len([str(x) for x in (ref.extra or {}).get("chain") or []])
        max_depth = max(1, int(self.config.archive.max_depth))
        if parent_depth + 1 > max_depth:
            raise UnsupportedFormatError(
                f"嵌套压缩包层数超过上限（{max_depth}）", reason="嵌套过深"
            )
        container = self._container_for(ref)
        try:
            entries = container.browse()
            target = next((x for x in entries if x.name == e), None)
            if target is None:
                raise AssetNotFoundError(f"压缩包内不存在条目: {e}")
            if target.is_dir:
                kind = "dir"
                image_count = sum(
                    1
                    for x in entries
                    if not x.is_dir and x.name.startswith(e + "/") and is_image_name(x.name)
                )
                min_images = max(1, int(self.config.gallery.min_images_in_folder))
                if image_count < min_images:
                    raise UnsupportedFormatError(
                        f"目录内图片数不足（{image_count} < {min_images}），无法作为图集打开",
                        reason="图片数不足",
                    )
                fmt = "gallery"
                page_count: Optional[int] = image_count
                title = stem_of(posixpath.basename(e))
            else:
                base = posixpath.basename(e)
                title = stem_of(base)
                page_count = None
                if is_image_name(base):
                    kind = "image"
                    fmt = "gallery"
                    page_count = 1
                else:
                    fmt = registry.format_of(base) or ""
                    if not fmt:
                        raise UnsupportedFormatError(
                            "包内不支持打开该文件", reason="不支持的文件类型"
                        )
                    reason = registry.openable_reason(fmt)
                    if reason:
                        raise UnsupportedFormatError(f"{fmt} 暂不可用：{reason}", reason=reason)
                    kind = "book"
        finally:
            try:
                container.close()
            except Exception:
                pass

        nested_id = nested_book_id(ref.book_id, e)
        self.register(
            nested_id,
            parent=ref.book_id,
            entry=e,
            kind=kind,
            format=fmt,
            title=title or e.rsplit("/", 1)[-1],
        )
        revision = nested_revision(nested_id, ref.revision)
        # 预览模式：由注册表能力决定（gallery/pdf/漫画/字体/DjVu… 都是固定版式），
        # 包内的压缩包条目按漫画容器打开也是固定版式。
        fixed = registry.is_fixed_format(fmt) or (kind == "book" and is_archive_name(e))
        return {
            "book_id": nested_id,
            "title": title or e.rsplit("/", 1)[-1],
            "format": fmt,
            "kind": kind,
            "mode": "fixed" if fixed else "flow",
            "page_count": page_count,
            "cover_url": f"/api/v1/books/{nested_id}/cover?rev={revision}",
            "depth": parent_depth + 1,
        }

    # ------------------------------------------------------------ 注册
    def register(
        self,
        nested_id: str,
        *,
        parent: str,
        entry: str,
        kind: str,
        format: str,
        title: str,
    ) -> None:
        with self._lock:
            prev = self._registry.get(nested_id)
            self._registry[nested_id] = {
                "parent": str(parent),
                "entry": str(entry),
                "kind": str(kind),
                "format": str(format),
                "title": str(title),
                "created_at": (prev or {}).get("created_at") or time.time(),
                "updated_at": time.time(),
            }
            self._prune_locked()
        self.save_registry()

    # ------------------------------------------------------------ 失效
    def invalidate_extracted(self, parent_book_id: str) -> int:
        """顶层压缩包内容变化：删除其链上全部嵌套书的解压缓存目录。

        注册表条目保留（resolve 会因父 id 失效而自然 404，避免在请求路径上
        做注册表重写）；真正过期条目由 LRU 淘汰。链要**上溯到根**：包中包里的
        条目记录的直接 parent 是内层嵌套书 id，不是顶层压缩包 id。
        """
        with self._lock:
            stale_ids = {
                str(k)
                for k, v in self._registry.items()
                if _root_parent(v, self._registry) == str(parent_book_id)
            }
        import shutil  # noqa: PLC0415

        removed = 0
        for nid in stale_ids:
            target = self.config.extract_cache_dir / str(nid)
            if target.is_dir():
                try:
                    shutil.rmtree(target, ignore_errors=True)
                    removed += 1
                except OSError:
                    continue
        return removed


def _root_parent(record: Dict[str, Any], registry: Dict[str, Dict[str, Any]]) -> str:
    """注册表记录的顶层父 id：沿 parent 链上溯到不在注册表的那一层（环防护）。"""
    current = record
    seen: set = set()
    while True:
        pid = str(current.get("parent") or "")
        if not pid or pid in seen:
            return pid
        seen.add(pid)
        nxt = registry.get(pid)
        if nxt is None:
            return pid
        current = nxt


def _asset_url(ref: BookRef, entry_name: str) -> str:
    from urllib.parse import quote  # noqa: PLC0415

    return (
        f"/api/v1/books/{ref.book_id}/asset?path={quote(entry_name, safe='')}"
        f"&rev={ref.revision}"
    )
