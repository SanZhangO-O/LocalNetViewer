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
from ..util.paths import PathError, ext_of, makedirs_quiet, normalize_vpath, stem_of
from ..util.textutil import content_revision
from ..formats.base import BookRef, UnsupportedFormatError
from ..formats.comics import list_folder_images

if TYPE_CHECKING:  # pragma: no cover
    from .library import LibraryService

GALLERY_PREFIX = "gallery:"
GALLERY_FORMAT = "gallery"
DEFAULT_SKIP = ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]

# 注册表上限：每次 open-folder 都可能新增一条，无上限时 galleries.json 会随
# 浏览过的目录数持续增长，且每次注册整文件重写的耗时 O(n) 同步上涨。
# 与 progress.json 的 5000 条上限同一策略：按最近更新时间淘汰。
MAX_REGISTRY = 5000

# 目录图片数枚举很贵（recursive 模式要遍历整棵子树），而 /books/{id}、封面与
# 翻页请求都会调 resolve()。按 book_id 记忆 (目录 mtime, 图片数, revision)，
# 目录 mtime 未变且距上次校验 < 2s 时复用；register/forget 主动失效。
GALLERY_MEMO_TTL_S = 2.0


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
    """枚举目录下的图片（相对路径，自然排序）；跳过隐藏与 skip_entries。

    实际枚举与缓存由 ``formats.comics.list_folder_images`` 提供：注册、目录计数
    与漫画容器的取页共用同一份结果，图集含上万张图时不会每个请求都重扫目录。
    """
    pats = list(patterns or DEFAULT_SKIP)
    return list_folder_images(Path(root), recursive, pats)


def count_images(root: Path, recursive: bool = False, patterns: Optional[List[str]] = None) -> int:
    return len(list_images(root, recursive, patterns))


class GalleryService:
    def __init__(self, config, library: "LibraryService") -> None:
        self.config = config
        self.library = library
        self.path = config.gallery_registry_path
        self._lock = threading.RLock()
        # 注册/落盘串行化：保证"取快照 + 写盘"期间没有第二个保存者插入，
        # 最后一次落盘必然包含之前所有注册（book_id 是哈希，覆盖丢条目无法反推）
        self._save_lock = threading.Lock()
        self._registry: Dict[str, Dict[str, Any]] = {}
        # resolve() 的短时记忆：{book_id: (dir_mtime, image_count, revision, checked_at)}
        self._memo: Dict[str, tuple] = {}
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
        # 快照 + 写盘在 _save_lock 内串行化（不会出现"旧快照后落盘"丢注册），
        # 但快照后写盘时不持 _lock：resolve()/翻页等热路径不被整文件重写阻塞。
        # tmp 名带进程 + 时间戳，进程内并发由 _save_lock 保证，跨进程也不互撞。
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

    def _prune_registry_locked(self) -> None:
        """注册表超过 MAX_REGISTRY 时按最近更新时间淘汰（调用方需持锁）。"""
        overflow = len(self._registry) - MAX_REGISTRY
        if overflow <= 0:
            return
        ordered = sorted(
            self._registry.items(),
            key=lambda item: float(item[1].get("updated_at", 0.0)),
        )
        for key, _ in ordered[:overflow]:
            self._registry.pop(key, None)
            self._memo.pop(key, None)

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
        # 封面 URL 带修订号（与服务端 rev 公式一致）：目录图片变化后 URL 变化
        try:
            revision = content_revision(
                book_id, len(images), float(real.stat().st_mtime)
            )
        except OSError:
            revision = book_id
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
            # 注册刚重新枚举过，旧记忆必须作废（recursive 标志可能也变了）
            self._memo.pop(book_id, None)
            self._prune_registry_locked()
            # 在同一临界区内取副本：否则并发 forget() 会让这里 KeyError
            entry = dict(self._registry[book_id])
        self.save_registry()
        return {
            "book_id": book_id,
            "title": entry["title"],
            "format": GALLERY_FORMAT,
            "page_count": len(images),
            "mode": "fixed",
            "cover_url": f"/api/v1/books/{book_id}/cover?rev={revision}",
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
        # 图集修订号契约：sha1("<id>:<图片数>:<目录 mtime 秒>")[:12]；
        # stat 失败时退化为 book_id。图片数必须实时枚举：
        # 目录 mtime 在 recursive 模式下不会随子目录增删改变，只有计数能兜住。
        # 但枚举很贵且 resolve() 每请求都调：目录 mtime 未变且 2s 内校验过则
        # 复用记忆的 (count, revision)；mtime 一变立即重算，register/forget 失效。
        try:
            dir_mtime = float(real.stat().st_mtime)
        except OSError:
            revision = book_id
        else:
            if real.is_file():
                count = 1
                revision = content_revision(book_id, count, dir_mtime)
            else:
                memo = None
                with self._lock:
                    cached = self._memo.get(book_id)
                if (
                    cached is not None
                    and abs(float(cached[0]) - dir_mtime) < 1e-6
                    and (time.monotonic() - float(cached[3])) < GALLERY_MEMO_TTL_S
                ):
                    memo = cached
                if memo is not None:
                    count, revision = int(memo[1]), str(memo[2])
                else:
                    count = count_images(
                        real,
                        bool(item.get("recursive", False)),
                        list(self.config.comic.skip_entries),
                    )
                    revision = content_revision(book_id, count, dir_mtime)
                    with self._lock:
                        self._memo[book_id] = (
                            dir_mtime,
                            count,
                            revision,
                            time.monotonic(),
                        )
        return BookRef(
            book_id=book_id,
            vpath=vpath,
            path=real,
            format=GALLERY_FORMAT,
            size=0,
            mtime=0.0,
            virtual=True,
            revision=revision,
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
            self._memo.pop(book_id, None)
        self.save_registry()
