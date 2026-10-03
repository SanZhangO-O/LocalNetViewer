"""文件夹图集（虚拟书，设计 7.5）。

- 入口：浏览目录时可把"当前目录"或"含图片的子目录"注册为虚拟漫画书，
  不需要打开任何具体文件；
- ``book_id = sha1("gallery:" + 虚拟路径)[:16]``，由目录路径生成 →
  目录内增删图片不改变 id（进度保留），目录移动/改名会失效；
- 注册表持久化在 ``cache/galleries.json``：book_id → {path, recursive, title}，
  因为 book_id 是哈希、无法反推路径（服务器重启后仍要能打开）；
- 图片枚举与自然排序与漫画容器共用同一套规则（``formats.comics``）。
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from ..config import IMAGE_EXT
from ..util.natural import natural_key
from ..util.paths import PathError, ext_of, makedirs_quiet, normalize_vpath, stem_of
from ..formats.base import BookRef, UnsupportedFormatError
from ..formats.comics import should_skip_entry

if TYPE_CHECKING:  # pragma: no cover
    from .library import LibraryService

GALLERY_PREFIX = "gallery:"
GALLERY_FORMAT = "gallery"
DEFAULT_SKIP = ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]


def gallery_book_id(vpath: str) -> str:
    """虚拟书 id（设计 4.8）。"""
    return hashlib.sha1((GALLERY_PREFIX + normalize_vpath(vpath, allow_empty=True)).encode("utf-8")).hexdigest()[:16]


def _is_image(name: str) -> bool:
    return ext_of(name) in set(IMAGE_EXT)


def list_images(
    root: Path,
    recursive: bool = False,
    patterns: Optional[List[str]] = None,
) -> List[str]:
    """枚举目录下的图片（相对路径，自然排序）；跳过隐藏与 skip_entries。"""
    pats = list(patterns or DEFAULT_SKIP)
    out: List[str] = []
    root = Path(root)
    if not root.is_dir():
        return out
    if recursive:
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = [
                d
                for d in dirnames
                if not d.startswith(".") and not os.path.islink(os.path.join(dirpath, d))
            ]
            rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")
            for fn in filenames:
                rel = fn if rel_dir in (".", "") else f"{rel_dir}/{fn}"
                if should_skip_entry(rel, pats) or not _is_image(rel):
                    continue
                # 与 FolderComicContainer._scan 保持一致：符号链接文件不计入页集合，
                # 否则列表显示 N 张、实际分页只有 N-1 张（read() 会拒绝越界软链）
                if os.path.islink(os.path.join(dirpath, fn)):
                    continue
                out.append(rel)
    else:
        try:
            entries = list(os.scandir(root))
        except OSError:
            return out
        for entry in entries:
            try:
                if entry.is_symlink() or not entry.is_file():
                    continue
            except OSError:
                continue
            if should_skip_entry(entry.name, pats) or not _is_image(entry.name):
                continue
            out.append(entry.name)
    return sorted(out, key=natural_key)


def count_images(root: Path, recursive: bool = False, patterns: Optional[List[str]] = None) -> int:
    return len(list_images(root, recursive, patterns))


class GalleryService:
    def __init__(self, config, library: "LibraryService") -> None:
        self.config = config
        self.library = library
        self.path = config.gallery_registry_path
        self._lock = threading.RLock()
        self._registry: Dict[str, Dict[str, Any]] = {}
        self.load_registry()

    # ------------------------------------------------------------ 注册表
    def load_registry(self) -> None:
        try:
            raw = self.path.read_text(encoding="utf-8")
            data = json.loads(raw)
            if isinstance(data, dict):
                self._registry = {
                    str(k): dict(v) for k, v in data.items() if isinstance(v, dict)
                }
        except FileNotFoundError:
            self._registry = {}
        except (OSError, json.JSONDecodeError, ValueError):
            self._registry = {}

    def save_registry(self) -> None:
        # 同一把锁内取快照并写盘：注册表很小，避免并发 register/forget 时
        # 写坏 galleries.json（损坏后 load_registry 会清空，所有虚拟书 id 失效）
        with self._lock:
            payload = json.dumps(self._registry, ensure_ascii=False, indent=1)
            tmp = self.path.with_suffix(".tmp")
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

    # ------------------------------------------------------------ 注册
    def register(self, vpath: str, recursive: Optional[bool] = None) -> Dict[str, Any]:
        """把目录（或单张图片）注册为虚拟图集书，返回概要。"""
        v = normalize_vpath(vpath)
        real = self.library.resolve_real(v)
        if real is None or not real.exists():
            raise PathError(f"路径不存在: {v}")

        rec = bool(self.config.gallery.recursive if recursive is None else recursive)
        patterns = list(self.config.comic.skip_entries)

        if real.is_file():
            if not _is_image(real.name):
                raise UnsupportedFormatError(
                    "该文件不是图片，无法作为图集打开", reason="非图片文件"
                )
            images = [real.name]
            title = stem_of(real.name)
            rec = False
        else:
            images = list_images(real, rec, patterns)
            title = real.name or v.rsplit("/", 1)[-1]

        min_images = int(self.config.gallery.min_images_in_folder)
        if len(images) < max(1, min_images):
            raise UnsupportedFormatError(
                f"目录内图片数不足（{len(images)} < {min_images}），无法作为图集打开",
                reason="图片数不足",
            )

        book_id = gallery_book_id(v)
        with self._lock:
            prev = self._registry.get(book_id)
            self._registry[book_id] = {
                "path": v,
                "recursive": rec,
                "title": title or (prev or {}).get("title") or v.rsplit("/", 1)[-1],
                "page_count": len(images),
                "created_at": (prev or {}).get("created_at") or time.time(),
                "updated_at": time.time(),
            }
            # 在同一临界区内取副本：否则并发 forget() 会让这里 KeyError
            entry = dict(self._registry[book_id])
        self.save_registry()
        return {
            "book_id": book_id,
            "title": entry["title"],
            "format": GALLERY_FORMAT,
            "page_count": len(images),
            "mode": "fixed",
            "cover_url": f"/api/v1/books/{book_id}/cover",
            "recursive": rec,
        }

    # ------------------------------------------------------------ 解析
    def entry(self, book_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            item = self._registry.get(book_id)
            return dict(item) if item else None

    def resolve(self, book_id: str) -> Optional[BookRef]:
        """book_id → 虚拟书 BookRef（用于 /books/{id}/* 全流程）。"""
        item = self.entry(book_id)
        if not item:
            return None
        vpath = normalize_vpath(str(item.get("path") or ""), allow_empty=True)
        real = self.library.resolve_real(vpath)
        if real is None or not real.exists():
            return None
        return BookRef(
            book_id=book_id,
            vpath=vpath,
            path=real,
            format=GALLERY_FORMAT,
            size=0,
            mtime=0.0,
            virtual=True,
            extra={"recursive": bool(item.get("recursive", False)), "title": item.get("title")},
        )

    def page_count(self, book_id: str) -> int:
        ref = self.resolve(book_id)
        if ref is None:
            return 0
        item = self.entry(book_id) or {}
        return count_images(
            ref.path,
            bool(item.get("recursive", False)),
            list(self.config.comic.skip_entries),
        )

    def forget(self, book_id: str) -> None:
        with self._lock:
            self._registry.pop(book_id, None)
        self.save_registry()
