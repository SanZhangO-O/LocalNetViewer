"""书库服务：扫描、虚拟路径、book_id 指纹、目录索引（设计 4.8 / 9）。

- 虚拟路径 ``/书库名/子目录/文件``；第一个路径段映射到配置的 root；
- 路径安全：解码后规范化 + ``resolve()`` 必须仍在 root 内，否则 PATH_TRAVERSAL；
  默认不跟随符号链接（软链逃逸出书库）；
- ``book_id = sha1(文件大小 + 文件头 1MB)[:16]``：书籍移动 / 改名后进度不丢；
- 索引落盘 ``cache/index.json``，按 mtime/size 跳过未变文件的指纹重算；
- 扫描在后台线程执行：未就绪时 ``/files`` 返回已索引部分并携带 ``scanning: true``；
- 目录图片数（图集入口用）在扫描时计算并按目录 mtime 缓存；
- ``/files`` 分页：枚举/排序是轻量活，stat / 指纹 / 子目录图片数只对**当前页**条目做，
  大目录翻页的成本与页大小而非目录大小成正比。
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from ..config import IMAGE_EXT, AppConfig
from ..formats import registry
from ..formats.base import BookRef
from ..formats.comics import should_skip_entry
from ..util.natural import natural_key
from ..util.paths import (
    PathError,
    join_vpath,
    ext_of,
    normalize_vpath,
    parent_vpath,
    resolve_in_root,
)
from ..util.textutil import content_revision
from . import gallery as gallery_mod

INDEX_VERSION = 1
HEAD_BYTES = 1 << 20  # 1MB


def compute_book_id(path: Path, size: Optional[int] = None) -> str:
    """book_id = sha1(文件大小 + 文件头 1MB) 取前 16 位（设计 4.8）。"""
    if size is None:
        try:
            size = int(os.path.getsize(path))
        except OSError:
            size = 0
    h = hashlib.sha1()
    h.update(str(int(size)).encode("ascii"))
    try:
        with open(path, "rb") as fh:
            h.update(fh.read(HEAD_BYTES))
    except OSError:
        pass
    return h.hexdigest()[:16]


def iso_time(ts: float) -> str:
    try:
        return (
            datetime.fromtimestamp(float(ts), tz=timezone.utc)
            .isoformat()
            .replace("+00:00", "Z")
        )
    except (OverflowError, OSError, ValueError):
        return ""


@dataclass
class DirGalleryInfo:
    image_count: int
    mtime: float

    def can_open(self, cfg: AppConfig) -> bool:
        return bool(cfg.gallery.folder_as_book) and self.image_count >= max(
            1, int(cfg.gallery.min_images_in_folder)
        )


class LibraryService:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        self.gallery: Optional[gallery_mod.GalleryService] = None  # 由 AppContext 注入
        self._lock = threading.RLock()
        self._scan_lock = threading.Lock()
        self._save_lock = threading.Lock()
        self._cancel_scan = threading.Event()
        self._books: Dict[str, Dict[str, Any]] = {}
        self._by_id: Dict[str, str] = {}
        self._gallery_counts: Dict[str, Dict[str, Any]] = {}
        self._scanned_at: float = 0.0
        # scanning 是跨线程读写的状态位（请求线程 / 扫描线程 / 关停线程）：用 Event
        # 而不是裸 bool，避免读到过期值（/files 的 scanning 字段必须准确）
        self._scanning = threading.Event()
        self._dirty: bool = False
        # 落盘代数：快照时 +1，_mark_dirty 时 +1。写盘成功后仅当代数未变
        # （期间没有新变更）才清 dirty，避免误清并发写入的标记（与 progress 同一套）
        self._generation: int = 0
        self._last_save: float = 0.0
        self._warnings: List[str] = []
        # 内容变化监听者：由 AppContext 注入"清空派生缓存"的回调
        self._content_listeners: List[Callable[[str], None]] = []

    # ------------------------------------------------------------ 内容变化通知
    def add_content_listener(self, callback: Callable[[str], None]) -> None:
        """注册"某本书内容已变化"的回调（AppContext 用它清理派生缓存）。"""
        if callback not in self._content_listeners:
            self._content_listeners.append(callback)

    def _notify_content_changed(self, book_id: str) -> None:
        for callback in list(self._content_listeners):
            try:
                callback(book_id)
            except Exception:  # noqa: BLE001 - 失效失败不能影响正在进行的请求
                continue

    # ------------------------------------------------------------ 索引
    def load_index(self) -> None:
        path = self.config.index_path
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, json.JSONDecodeError, ValueError):
            self._warnings.append(f"索引文件损坏，已忽略: {path}")
            return
        if int(data.get("version", 0)) != INDEX_VERSION:
            return
        with self._lock:
            books = {
                str(k): dict(v)
                for k, v in (data.get("books") or {}).items()
                if isinstance(v, dict) and v.get("book_id")
            }
            self._books = books
            self._by_id = {str(v["book_id"]): k for k, v in books.items()}
            self._gallery_counts = {
                str(k): dict(v) for k, v in (data.get("gallery_counts") or {}).items()
            }
            self._scanned_at = float(data.get("scanned_at") or 0.0)

    def save_index(self) -> None:
        # _save_lock 串行化"取快照 + 落盘"，保证最后一次保存一定是较新的状态；
        # 序列化在 _lock 内完成：拿到的只是字典引用，锁外 dump 会撞上
        # 请求线程的 _book_id_for 插入（RuntimeError: dictionary changed size）
        with self._save_lock:
            with self._lock:
                payload_text = json.dumps(
                    {
                        "version": INDEX_VERSION,
                        "scanned_at": self._scanned_at,
                        "books": self._books,
                        "gallery_counts": self._gallery_counts,
                    },
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
                self._last_save = time.time()
                self._generation += 1
                generation = self._generation
            # dirty 只在写盘**成功后**清除：写盘失败（磁盘满等）时保留 dirty，
            # 让下一次 maybe_save / 关停 flush 还能把这批变更落盘，进程崩溃前
            # 至少多一次机会。旧实现先清 dirty 再写盘，失败即静默丢更新。
            ok = False
            tmp = self.config.index_path.with_suffix(f".tmp{os.getpid()}.{time.time_ns()}")
            try:
                self.config.index_path.parent.mkdir(parents=True, exist_ok=True)
                tmp.write_text(payload_text, encoding="utf-8")
                os.replace(tmp, self.config.index_path)
                ok = True
            except OSError:
                try:
                    tmp.unlink(missing_ok=True)
                except OSError:
                    pass
            finally:
                with self._lock:
                    if ok:
                        # 快照后没有新变更才清 dirty；否则保留给下一次落盘
                        if self._generation == generation:
                            self._dirty = False
                    else:
                        self._dirty = True

    def _mark_dirty(self) -> None:
        with self._lock:
            self._dirty = True
            self._generation += 1

    def maybe_save(self, min_interval: float = 30.0) -> None:
        with self._lock:
            if not self._dirty or (time.time() - self._last_save) < min_interval:
                return
        self.save_index()

    # ------------------------------------------------------------ 扫描
    @property
    def scanning(self) -> bool:
        return self._scanning.is_set()

    @property
    def warnings(self) -> List[str]:
        return list(self._warnings)

    def book_count(self) -> int:
        with self._lock:
            return len(self._books)

    def libraries_info(self) -> List[Dict[str, Any]]:
        out: List[Dict[str, Any]] = []
        # 直接在锁内按 root 计数：旧实现每次 /server/info 都 dict(self._books)
        # 复制整份索引（每本书一个 dict），书库大时纯属浪费
        with self._lock:
            for root in self.config.library.roots:
                prefix = f"/{root.name}"
                count = sum(
                    1
                    for vp in self._books
                    if vp == prefix or vp.startswith(prefix + "/")
                )
                out.append(
                    {"name": root.name, "book_count": count, "path": root.path}
                )
        return out

    def scan_blocking(self) -> bool:
        """全量/增量扫描（同步，供启动预热与后台线程调用）。

        返回 False 表示 ``_scan_lock`` 被占用而**跳过**：watcher 等事件驱动的
        调用方必须据此重试——watchfiles 不会重新投递已消费的事件，静默跳过
        等于丢掉那批变化的重扫机会。
        """
        if not self._scan_lock.acquire(blocking=False):
            return False
        self._scanning.set()
        self._cancel_scan.clear()
        changed_old_ids: List[str] = []
        try:
            with self._lock:
                prev_books = dict(self._books)
                prev_counts = {
                    k: dict(v) for k, v in self._gallery_counts.items()
                }
            books: Dict[str, Dict[str, Any]] = {}
            counts: Dict[str, Dict[str, Any]] = {}
            warnings: List[str] = []
            for root in self.config.library.roots:
                root_path = Path(root.path)
                if not root_path.is_dir():
                    warnings.append(
                        f"书库根目录不存在，已跳过: {root.name} ({root.path})"
                    )
                    continue
                self._scan_root(
                    root, root_path, prev_books, books, counts, changed_old_ids
                )
            if self.config.gallery.recursive:
                _rollup_gallery_counts(counts)
            with self._lock:
                self._books = books
                self._by_id = {str(v["book_id"]): k for k, v in books.items()}
                self._gallery_counts = counts
                self._scanned_at = time.time()
                self._warnings = warnings
                # 只有索引内容真的变化才标 dirty：watcher 的事件驱动重扫可能每几秒
                # 一次，no-op 扫描不能跟着做一次全量 json.dumps（在锁内，会卡请求
                # 线程）+ 全文件重写。请求线程自行标的 dirty 保持不变。
                if books != prev_books or counts != prev_counts:
                    self._dirty = True
        finally:
            self._scanning.clear()
            self._scan_lock.release()
        # 内容变化通知必须在锁外：监听者会清磁盘/内存缓存（可能做 IO），
        # 持锁通知容易与请求线程互等；每个旧 book_id 只通知一次
        for old_id in dict.fromkeys(changed_old_ids):
            self._notify_content_changed(old_id)
        if self._dirty:
            self.save_index()
        return True

    async def scan_async(self) -> bool:
        return await asyncio.to_thread(self.scan_blocking)

    def cancel_scan(self) -> None:
        """请求停止后台扫描（线程无法被强杀，只能协作式退出）。"""
        self._cancel_scan.set()

    def wait_idle(self, timeout: float = 5.0) -> bool:
        """等待扫描线程真正结束（关停时先等它停，再保存索引，避免互相覆盖）。"""
        deadline = time.time() + max(0.0, timeout)
        while time.time() < deadline:
            if self._scan_lock.acquire(blocking=False):
                self._scan_lock.release()
                return True
            if not self._scanning.is_set():
                return True
            time.sleep(0.1)
        return not self._scanning.is_set()

    def _scan_root(
        self,
        root,
        root_path: Path,
        prev_books: Dict[str, Dict[str, Any]],
        books: Dict[str, Dict[str, Any]],
        counts: Dict[str, Dict[str, Any]],
        changed_old_ids: List[str],
    ) -> None:
        patterns = list(self.config.comic.skip_entries)
        for dirpath, dirnames, filenames in os.walk(root_path, followlinks=False):
            if self._cancel_scan.is_set():
                # 关停 / 重新扫描：尽快退出，让出锁给保存索引
                return
            dirnames[:] = sorted(
                [
                    d
                    for d in dirnames
                    if not d.startswith(".")
                    and not os.path.islink(os.path.join(dirpath, d))
                ],
                key=natural_key,
            )
            current = Path(dirpath)
            rel_dir = os.path.relpath(dirpath, root_path).replace("\\", "/")
            rel_parts = [] if rel_dir in (".", "") else rel_dir.split("/")
            vdir = join_vpath(f"/{root.name}", *rel_parts) if rel_parts else f"/{root.name}"

            try:
                dir_mtime = float(current.stat().st_mtime)
            except OSError:
                dir_mtime = 0.0

            image_count = 0
            for fn in sorted(filenames, key=natural_key):
                if fn.startswith("."):
                    continue
                real = current / fn
                try:
                    if real.is_symlink() or not real.is_file():
                        continue
                    st = real.stat()
                except OSError:
                    continue
                ext = ext_of(fn)
                if ext in set(IMAGE_EXT) and not should_skip_entry(fn, patterns):
                    image_count += 1
                    continue
                fmt = registry.format_of(fn)
                if not fmt:
                    continue
                if (
                    ext not in self.config.library.include_ext
                    and not self.config.library.show_all_files
                ):
                    continue
                vpath = join_vpath(vdir, fn)
                prev = prev_books.get(vpath)
                if (
                    prev
                    and int(prev.get("size", -1)) == int(st.st_size)
                    and abs(float(prev.get("mtime", 0.0)) - float(st.st_mtime)) < 1e-6
                    and prev.get("format") == fmt
                    and prev.get("book_id")
                ):
                    book_id = str(prev["book_id"])
                else:
                    book_id = compute_book_id(real, int(st.st_size))
                    # 实际内容变化：登记旧 id，扫描提交后在锁外通知监听者
                    # （旧实现静默吞掉变化，派生缓存永远不会失效）
                    old_id = str(prev.get("book_id") or "") if prev else ""
                    if old_id and self._claim_scan_change(
                        vpath,
                        old_id,
                        book_id,
                        fmt,
                        int(st.st_size),
                        float(st.st_mtime),
                        prev_size=int(prev.get("size", -1)),
                        prev_mtime=float(prev.get("mtime", 0.0)),
                    ):
                        changed_old_ids.append(old_id)
                books[vpath] = {
                    "book_id": book_id,
                    "format": fmt,
                    "size": int(st.st_size),
                    "mtime": float(st.st_mtime),
                }

            counts[vdir] = {"count": image_count, "mtime": dir_mtime}

    def _claim_scan_change(
        self,
        vpath: str,
        old_id: str,
        book_id: str,
        fmt: str,
        size: int,
        mtime: float,
        *,
        prev_size: int,
        prev_mtime: float,
    ) -> bool:
        """扫描检测到内容变化时，把 ``_books[vpath]`` 从旧 id 切换到新记录。

        返回 True 表示这次变化由扫描负责通知旧 id；若请求线程的 ``_book_id_for``
        已抢先切换并通知过，则返回 False，避免同一次变化通知两次。别名清理与
        ``_book_id_for`` 一致：立即摘掉旧别名、登记新 id，扫描结束时会整体重建
        ``_by_id``。

        只比较 book_id 是不够的：1MB 之后的等长编辑指纹不变，请求线程处理完
        live 记录的 book_id 仍是旧 id，但 size/mtime 已刷新为新值。必须同时
        要求 live 仍停留在**变化前**的 size/mtime，否则这次变化已经有人通知过。
        """
        with self._lock:
            live = self._books.get(vpath)
            if live is None or str(live.get("book_id") or "") != old_id:
                return False
            if (
                int(live.get("size", -1)) != int(prev_size)
                or abs(float(live.get("mtime", 0.0)) - float(prev_mtime)) >= 1e-6
            ):
                return False
            self._books[vpath] = {
                "book_id": book_id,
                "format": fmt,
                "size": int(size),
                "mtime": float(mtime),
            }
            if old_id != book_id and self._by_id.get(old_id) == vpath:
                self._by_id.pop(old_id, None)
            self._by_id[book_id] = vpath
            return True

    # ------------------------------------------------------------ 目录图片数
    def dir_gallery_info(self, vpath: str, real: Optional[Path] = None) -> DirGalleryInfo:
        """目录图片数（命中索引缓存则不重新遍历）。

        ``vpath`` 可能来自文件系统（列表里的子目录名），而 ``normalize_vpath`` 会拒绝
        含 ``:`` 等字符的段（Windows 盘符 / NTFS 数据流防护）。这种目录名在
        Linux/SMB 上是合法的，**不能因为它让整个父目录的列表 400**：这里退化为
        用原始字符串做缓存键。
        """
        try:
            v = normalize_vpath(vpath, allow_empty=True)
        except PathError:
            v = str(vpath)
        if real is None:
            try:
                real = self.resolve_real(v)
            except PathError:
                real = None
        mtime = 0.0
        if real is not None:
            try:
                mtime = float(real.stat().st_mtime)
            except OSError:
                mtime = 0.0
        with self._lock:
            cached = self._gallery_counts.get(v)
        # gallery.recursive=true 时缓存的是"整棵子树"的计数，而校验只用目录自身 mtime，
        # 子目录变化不会改变父目录 mtime → 直接不使用缓存，保证计数正确
        if (
            not self.config.gallery.recursive
            and cached is not None
            and abs(float(cached.get("mtime", -1)) - mtime) < 1e-6
        ):
            return DirGalleryInfo(image_count=int(cached.get("count", 0)), mtime=mtime)
        count = 0
        if real is not None and real.is_dir():
            count = gallery_mod.count_images(
                real,
                bool(self.config.gallery.recursive),
                list(self.config.comic.skip_entries),
            )
        with self._lock:
            self._gallery_counts[v] = {"count": count, "mtime": mtime}
            self._dirty = True
        return DirGalleryInfo(image_count=count, mtime=mtime)

    # ------------------------------------------------------------ 路径解析
    def root_names(self) -> List[str]:
        return [r.name for r in self.config.library.roots]

    def resolve_real(self, vpath: str) -> Optional[Path]:
        """虚拟路径 → 真实路径（含穿越校验）。根名未知返回 None；不要求存在。"""
        v = normalize_vpath(vpath, allow_empty=True)
        parts = [p for p in v.split("/") if p]
        if not parts:
            return None
        root = self.config.root_by_name(parts[0])
        if root is None:
            return None
        return resolve_in_root(Path(root.path), parts[1:])

    def root_name_of(self, vpath: str) -> Optional[str]:
        parts = [p for p in normalize_vpath(vpath, allow_empty=True).split("/") if p]
        return parts[0] if parts else None

    def rel_of(self, vpath: str) -> str:
        parts = [p for p in normalize_vpath(vpath, allow_empty=True).split("/") if p]
        return "/".join(parts[1:])

    # ------------------------------------------------------------ 目录列表
    def list_dir(self, vpath: str, limit: int = 1000, offset: int = 0) -> Dict[str, Any]:
        """目录列表（分页）。

        分成两个阶段，让分页不仅裁剪响应体，也裁剪服务器的每页成本：

        - 第一阶段（轻量）：只 scandir 一次，按**名字**分类 + 自然排序 + 切页。
          不 stat、不算 book_id 指纹（每文件要读 1MB）、不数子目录图片；
        - 第二阶段（重活）：只对**当前页**的条目 stat / 指纹 / 数图片。

        旧实现把重活做在全目录上再切片，目录里有几万个文件时翻到第 2 页
        也要把整个目录重新扫一遍（未索引文件逐个读头算指纹），分页形同虚设。
        未列入当前页的文件指纹由后台扫描 / watcher 负责补齐（见模块 docstring）。
        """
        v = normalize_vpath(vpath, allow_empty=True)
        if v == "/":
            return self._list_roots(limit, offset)

        root_name = self.root_name_of(v)
        if root_name is None or self.config.root_by_name(root_name) is None:
            raise PathError(f"书库不存在: {root_name or v}")
        real = self.resolve_real(v)
        if real is None or not real.exists():
            raise PathError(f"路径不存在: {v}")
        if not real.is_dir():
            raise PathError(f"不是目录: {v}")

        # ---- 第一阶段：枚举 + 分类（只看名字）+ 排序
        patterns = list(self.config.comic.skip_entries)
        image_exts = set(IMAGE_EXT)
        show_images = bool(self.config.gallery.show_image_files)
        show_all = bool(self.config.library.show_all_files)
        include_ext = self.config.library.include_ext

        try:
            scandir_entries = list(os.scandir(real))
        except OSError as exc:
            raise PathError(f"目录无法读取: {v} ({exc})") from exc

        dirs: List[Tuple[str, str]] = []  # (name, 真实路径)
        # kind: "image"（单图）/ "book"（电子书）/ "file"（show_all_files 的其它文件）
        files: List[Tuple[str, str, str]] = []
        for entry in scandir_entries:
            name = entry.name
            if name.startswith("."):
                continue
            try:
                if entry.is_symlink():
                    continue
                is_dir = entry.is_dir()
            except OSError:
                continue
            if is_dir:
                dirs.append((name, entry.path))
                continue
            ext = ext_of(name)
            if ext in image_exts and show_images:
                if not should_skip_entry(name, patterns):
                    files.append((name, entry.path, "image"))
                continue
            fmt = registry.format_of(name)
            if fmt is None:
                if show_all:
                    files.append((name, entry.path, "file"))
                continue
            if ext not in include_ext and not show_all:
                continue
            files.append((name, entry.path, "book"))

        dirs.sort(key=lambda e: natural_key(e[0]))
        files.sort(key=lambda e: natural_key(e[0]))
        listing: List[Tuple[str, str, str]] = [
            (name, path, "dir") for name, path in dirs
        ] + files

        total = len(listing)
        start = max(0, int(offset))
        page = listing[start : start + max(1, int(limit))]

        # ---- 第二阶段：只对页内条目做重活（stat / 指纹 / 子目录图片数）
        entries: List[Dict[str, Any]] = []
        for name, child_real, kind in page:
            child_vpath = join_vpath(v, name)
            if kind == "dir":
                info = self.dir_gallery_info(child_vpath, Path(child_real))
                # 名字本身无法表示成虚拟路径（含 ':' 等）时明确置灰，
                # 而不是让用户点进去才拿到一个 400
                navigable = True
                reason = None
                try:
                    normalize_vpath(child_vpath)
                except PathError as exc:
                    navigable = False
                    reason = f"目录名服务器不支持：{exc}"
                entries.append(
                    {
                        "name": name,
                        "type": "dir",
                        "path": child_vpath,
                        "gallery": {
                            "image_count": info.image_count,
                            "can_open_as_book": info.can_open(self.config) and navigable,
                        },
                        "openable": navigable,
                        "reason": reason,
                    }
                )
                continue

            try:
                st = os.stat(child_real)
            except OSError:
                # 文件恰在列出后消失：该条跳过（本页可能短于 limit），不影响 total
                continue

            if kind == "image":
                min_images = max(1, int(self.config.gallery.min_images_in_folder))
                # 单张图片按"单页图集"打开，但服务器要求图片数 ≥ min_images_in_folder，
                # 所以只有该阈值为 1 时才能真的打开（否则列表不能谎报可打开）
                can_open = bool(self.config.gallery.folder_as_book) and min_images <= 1
                if not self.config.gallery.folder_as_book:
                    reason = "图集功能已关闭"
                elif not can_open:
                    reason = f"单张图片需要 gallery.min_images_in_folder ≤ 1（当前 {min_images}）"
                else:
                    reason = None
                entries.append(
                    {
                        "name": name,
                        "type": "image",
                        "path": child_vpath,
                        "format": "gallery",
                        "book_id": gallery_mod.gallery_book_id(child_vpath),
                        "size": int(st.st_size),
                        "mtime": iso_time(st.st_mtime),
                        "gallery": {
                            "image_count": 1,
                            "can_open_as_book": can_open,
                        },
                        "openable": can_open,
                        "reason": reason,
                    }
                )
                continue

            if kind == "file":
                entries.append(
                    {
                        "name": name,
                        "type": "file",
                        "path": child_vpath,
                        "size": int(st.st_size),
                        "mtime": iso_time(st.st_mtime),
                        "openable": False,
                        "reason": "非电子书格式",
                    }
                )
                continue

            fmt = registry.format_of(name) or ""
            book_id = self._book_id_for(
                child_vpath, Path(child_real), st.st_size, st.st_mtime, fmt
            )
            # 封面 URL 带内容修订号：内容变化后 URL 变化，客户端强缓存自然失效
            rev = content_revision(book_id, int(st.st_size), float(st.st_mtime))
            reason = registry.openable_reason(fmt)
            entries.append(
                {
                    "name": name,
                    "type": "book",
                    "path": child_vpath,
                    "format": fmt,
                    "book_id": book_id,
                    "size": int(st.st_size),
                    "mtime": iso_time(st.st_mtime),
                    "cover_url": f"/api/v1/books/{book_id}/cover?rev={rev}",
                    "openable": reason is None,
                    "reason": reason,
                }
            )

        gallery_info = self.dir_gallery_info(v, real)
        return {
            "path": v,
            "parent": parent_vpath(v),
            "libraries": None,
            "gallery": {
                "image_count": gallery_info.image_count,
                "can_open_as_book": gallery_info.can_open(self.config),
            },
            "entries": entries,
            "total": total,
            "limit": int(limit),
            "offset": int(offset),
            "scanning": self._scanning.is_set(),
        }

    def _list_roots(self, limit: int, offset: int) -> Dict[str, Any]:
        entries: List[Dict[str, Any]] = []
        for root in self.config.library.roots:
            exists = Path(root.path).is_dir()
            entries.append(
                {
                    "name": root.name,
                    "type": "dir",
                    "path": f"/{root.name}",
                    "gallery": None,
                    "openable": exists,
                    "reason": None if exists else "书库根目录不存在",
                }
            )
        total = len(entries)
        page = entries[max(0, offset) : max(0, offset) + max(1, limit)]
        return {
            "path": "/",
            "parent": None,
            "libraries": self.libraries_info(),
            "gallery": None,
            "entries": page,
            "total": total,
            "limit": int(limit),
            "offset": int(offset),
            "scanning": self._scanning.is_set(),
        }

    def _book_id_for(
        self, vpath: str, real: Path, size: int, mtime: float, fmt: str
    ) -> str:
        with self._lock:
            info = self._books.get(vpath)
            if (
                info
                and int(info.get("size", -1)) == int(size)
                and abs(float(info.get("mtime", 0.0)) - float(mtime)) < 1e-6
                and info.get("format") == fmt
            ):
                return str(info["book_id"])
        book_id = compute_book_id(real, size)
        with self._lock:
            # 并发重复检测：另一个线程（或扫描）已经完成本次切换并通知过旧 id，
            # 直接沿用其结果，避免同一次变化通知两次
            info = self._books.get(vpath)
            if (
                info
                and int(info.get("size", -1)) == int(size)
                and abs(float(info.get("mtime", 0.0)) - float(mtime)) < 1e-6
                and info.get("format") == fmt
            ):
                return str(info["book_id"])
            # 同一个 vpath 换了内容 → 旧 book_id 必须彻底作废，否则 _by_id 里会留下
            # 一个"指向同一路径、却声称自己是旧 id"的别名：客户端拿着旧 id 请求时，
            # 会拿到新文件的内容（size/mtime 都已刷新，检测不到任何变化）
            previous = str(info.get("book_id") or "") if info else ""
            self._books[vpath] = {
                "book_id": book_id,
                "format": fmt,
                "size": int(size),
                "mtime": float(mtime),
            }
            if previous and previous != book_id and self._by_id.get(previous) == vpath:
                self._by_id.pop(previous, None)
            self._by_id[book_id] = vpath
            self._dirty = True
        if previous:
            # 内容变化通知必须在锁外：监听者会清磁盘/内存缓存（可能做 IO）。
            # previous 未必等于 book_id（1MB 之后的等长编辑指纹不变），
            # 只要检测到变化就丢弃该 id 的全部派生缓存。
            self._notify_content_changed(previous)
        return book_id

    # ------------------------------------------------------------ 书籍引用
    def book_by_id(self, book_id: str) -> Optional[BookRef]:
        bid = str(book_id or "").strip()
        if not bid:
            return None
        with self._lock:
            vpath = self._by_id.get(bid)
            info = dict(self._books.get(vpath, {})) if vpath else {}
        if vpath:
            real = self.resolve_real(vpath)
            if real is not None and real.is_file():
                fmt = str(info.get("format") or registry.format_of(vpath) or "")
                # 索引里记录的 book_id 已经不是请求的这个 → 这个 id 是被替换掉的旧指纹。
                # 不能继续按 vpath 提供文件：那等于用旧 id 交付新内容，而且 size/mtime
                # 都已刷新，后面的"变化检测"再也发现不了。
                if info and str(info.get("book_id") or "") not in ("", bid):
                    return None
                if info:
                    # 文件可能被"就地替换"：book_id 是内容指纹，尺寸/时间变了就必须重算，
                    # 否则会拿旧 PageMap 去渲染新内容（页码/锚点全错）
                    try:
                        st = real.stat()
                    except OSError:
                        return None
                    if (
                        int(info.get("size", -1)) != int(st.st_size)
                        or abs(float(info.get("mtime", 0.0)) - float(st.st_mtime)) > 1e-6
                    ):
                        refreshed = self._book_id_for(
                            vpath, real, int(st.st_size), float(st.st_mtime), fmt
                        )
                        # 内容变化通知已由 _book_id_for 在锁外统一发出（旧 id 一次），
                        # 这里只决定旧 id 是否还能继续交付
                        # book_id 只覆盖"大小 + 文件头 1MB"，1MB 之后的等长编辑
                        # （TXT 勘误的常见形态）算出来的指纹和旧的一样。此时若只更新
                        # size/mtime 就继续用旧缓存，会永远读到旧分页与旧页面图。
                        # 所以：只要检测到文件变化就丢弃该 book_id 的全部派生缓存。
                        if refreshed != bid:
                            # 内容指纹真的变了：旧 id 已失效，客户端需重新扫描
                            return None
                        info = {"size": int(st.st_size), "mtime": float(st.st_mtime)}
                else:
                    # 防御：_by_id 有记录但 _books 无详情（索引不一致）时补一次 stat
                    try:
                        st = real.stat()
                    except OSError:
                        st = None
                    if st is not None:
                        info = {"size": int(st.st_size), "mtime": float(st.st_mtime)}
                size = int(info.get("size", 0))
                mtime = float(info.get("mtime", 0.0))
                return BookRef(
                    book_id=bid,
                    vpath=vpath,
                    path=real,
                    format=fmt,
                    size=size,
                    mtime=mtime,
                    virtual=False,
                    revision=content_revision(bid, size, mtime),
                )
        if self.gallery is not None:
            ref = self.gallery.resolve(bid)
            if ref is not None:
                return ref
        return None

    def book_by_vpath(self, vpath: str) -> Optional[BookRef]:
        v = normalize_vpath(vpath)
        with self._lock:
            info = dict(self._books.get(v) or {})
        real = self.resolve_real(v)
        if real is None or not real.is_file():
            return None
        fmt = str(info.get("format") or registry.format_of(v) or "")
        book_id = str(info.get("book_id") or "") or compute_book_id(real)
        if info:
            size = int(info.get("size", 0))
            mtime = float(info.get("mtime", 0.0))
        else:
            try:
                st = real.stat()
                size = int(st.st_size)
                mtime = float(st.st_mtime)
            except OSError:
                size = 0
                mtime = 0.0
        return BookRef(
            book_id=book_id,
            vpath=v,
            path=real,
            format=fmt,
            size=size,
            mtime=mtime,
            virtual=False,
            revision=content_revision(book_id, size, mtime),
        )

    def dir_ref(self, vpath: str, recursive: Optional[bool] = None) -> Optional[BookRef]:
        """目录 → 虚拟图集 BookRef（未注册时返回 None，由 GalleryService 注册）。"""
        if self.gallery is None:
            return None
        v = normalize_vpath(vpath)
        book_id = gallery_mod.gallery_book_id(v)
        return self.gallery.resolve(book_id)

    # ------------------------------------------------------------ 统计
    def index_stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "books": len(self._books),
                "gallery_dirs": len(self._gallery_counts),
                "scanned_at": self._scanned_at,
                "scanning": self._scanning.is_set(),
                "warnings": list(self._warnings),
            }


def _rollup_gallery_counts(counts: Dict[str, Dict[str, Any]]) -> None:
    """gallery.recursive=true 时把子目录图片数累加到父目录（自底向上）。"""
    ordered = sorted(counts.keys(), key=lambda p: p.count("/"), reverse=True)
    for path in ordered:
        if path == "/":
            continue
        parent = path.rsplit("/", 1)[0] or "/"
        parent_entry = counts.get(parent)
        if parent_entry is None:
            continue
        parent_entry["count"] = int(parent_entry.get("count", 0)) + int(
            counts[path].get("count", 0)
        )
