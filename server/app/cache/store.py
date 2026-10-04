"""布局缓存：LayoutProfile + PageMap（设计 3.4 / 4.6）。

- 键：``book_id + layout_id``；
- 内存：``cache.layout_memory_max`` 条 LRU；
- 磁盘：``cache/layouts/{book_id}/{layout_id}.json.gz``，服务器重启后 layout_id
  依然有效（客户端无需重新布局，设计 4.6 / 风险 8）；
- 文件内同时保存 LayoutProfile：``/pages?layout_id=`` 只带 layout_id 也要能渲染。
"""

from __future__ import annotations

import gzip
import json
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional

from ..layout.engine import PageMap
from ..models.schemas import LayoutProfile
from ..util.paths import is_safe_id
from .lru import CountLRU

# 读命中续 mtime 的冷却时间：磁盘 LRU 看"最近使用"，但每次读都 utime
# 会让每个页面请求都产生一次 inode 写入；60s 内的重复读不再 touch。
_TOUCH_COOLDOWN_S = 60.0


@dataclass
class LayoutEntry:
    book_id: str
    layout_id: str
    profile: LayoutProfile
    pagemap: PageMap

    @property
    def mode(self) -> str:
        return self.pagemap.mode


class LayoutStore:
    def __init__(self, config) -> None:
        self.config = config
        self.dir: Path = config.layouts_cache_dir
        self.memory = CountLRU(max_items=int(config.cache.layout_memory_max))
        self._lock = threading.RLock()
        self._save_lock = threading.Lock()
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass

    # ------------------------------------------------------------ 路径
    def dir_for(self, book_id: str) -> Optional[Path]:
        # book_id 直接作为目录名：非法值（分隔符 / ".." / 盘符）返回 None，
        # 调用方不得再构造或删除任何路径
        if not is_safe_id(book_id):
            return None
        return self.dir / book_id

    def path_for(self, book_id: str, layout_id: str) -> Optional[Path]:
        directory = self.dir_for(book_id)
        if directory is None or not is_safe_id(layout_id):
            return None
        return directory / f"{layout_id}.json.gz"

    def _key(self, book_id: str, layout_id: str) -> tuple:
        return (book_id, layout_id)

    # ------------------------------------------------------------ 读取
    def load_disk(self, book_id: str, layout_id: str) -> Optional[LayoutEntry]:
        path = self.path_for(book_id, layout_id)
        if path is None:
            return None
        try:
            with gzip.open(path, "rt", encoding="utf-8") as fh:
                data = json.load(fh)
        except FileNotFoundError:
            return None
        except (OSError, ValueError, EOFError, gzip.BadGzipFile):
            # 损坏缓存（含 UnicodeDecodeError）直接丢弃
            self._quarantine(path)
            return None
        # 校验身份：拿到别的书/别的 layout 的 PageMap 比没有缓存更糟
        if str(data.get("book_id") or "") not in ("", book_id):
            self._quarantine(path)
            return None
        if str(data.get("layout_id") or "") not in ("", layout_id):
            self._quarantine(path)
            return None
        try:
            profile = LayoutProfile.model_validate(data.get("profile") or {})
            pagemap = PageMap.from_dict(data.get("pagemap") or {})
        except Exception:
            self._quarantine(path)
            return None
        if not pagemap.layout_id:
            pagemap.layout_id = layout_id
        self._touch(path)
        return LayoutEntry(book_id=book_id, layout_id=layout_id, profile=profile, pagemap=pagemap)

    @staticmethod
    def _touch(path) -> None:
        """读命中即更新 mtime：磁盘 LRU 按"最近使用"而不是"最近写入"淘汰。

        否则热布局（频繁读取但写入很早）会先于冷布局被清掉。
        节流：mtime 还年轻（< _TOUCH_COOLDOWN_S）时跳过 utime，避免每个
        页面请求都写一次 inode。
        """
        try:
            if time.time() - path.stat().st_mtime < _TOUCH_COOLDOWN_S:
                return
            os.utime(path)
        except OSError:
            pass

    @staticmethod
    def _quarantine(path) -> None:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass

    def get(self, book_id: str, layout_id: str) -> Optional[LayoutEntry]:
        """内存 → 磁盘。

        磁盘读取（gzip + JSON，几 MB 的 PageMap 可能耗时数十毫秒）**必须在锁外**：
        早期版本把整段放在全局锁里，导致所有书的页面请求被串行化——一本大书的
        冷启动会拖住其他客户端的翻页（设计 4.9）。
        """
        if not (is_safe_id(book_id) and is_safe_id(layout_id)):
            return None
        key = self._key(book_id, layout_id)
        with self._lock:
            entry = self.memory.get(key)
        if entry is not None:
            return entry

        entry = self.load_disk(book_id, layout_id)
        if entry is None:
            return None
        with self._lock:
            # 并发加载同一 layout 时以先写入内存的为准，避免重复 put 触发多余淘汰
            existing = self.memory.get(key)
            if existing is not None:
                return existing
            self.memory.put(key, entry)
        return entry

    def exists(self, book_id: str, layout_id: str) -> bool:
        return self.get(book_id, layout_id) is not None

    # ------------------------------------------------------------ 写入
    def save(self, entry: LayoutEntry, *, enforce_quota: bool = True) -> None:
        path = self.path_for(entry.book_id, entry.layout_id)
        if path is None:
            return  # 非法 id：不落盘、不构造路径
        with self._lock:
            self.memory.put(self._key(entry.book_id, entry.layout_id), entry)
        tmp = path.with_suffix(path.suffix + ".tmp")
        payload = {
            "book_id": entry.book_id,
            "layout_id": entry.layout_id,
            "profile": entry.profile.model_dump(mode="json"),
            "pagemap": entry.pagemap.to_dict(),
        }
        with self._save_lock:
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                with gzip.open(tmp, "wt", encoding="utf-8") as fh:
                    json.dump(payload, fh, ensure_ascii=False, separators=(",", ":"))
                os.replace(tmp, path)
            except OSError:
                try:
                    tmp.unlink(missing_ok=True)
                except OSError:
                    pass
        if enforce_quota:
            self.enforce_quota(keep=path)

    def enforce_quota(self, keep: Optional[Path] = None) -> int:
        """磁盘布局缓存按配额 LRU 淘汰（设计 4.6：LRU 淘汰策略保证磁盘可控）。"""
        quota = int(self.config.cache.layouts_max_mb or 0) * 1024 * 1024
        if quota <= 0:
            return 0
        entries = []
        total = 0
        try:
            for book_dir in self.dir.iterdir():
                if not book_dir.is_dir():
                    continue
                for path in book_dir.glob("*.json.gz"):
                    try:
                        st = path.stat()
                    except OSError:
                        continue
                    entries.append((st.st_mtime, st.st_size, path))
                    total += st.st_size
        except OSError:
            return 0
        if total <= quota:
            return 0
        target = int(quota * 0.8)
        entries.sort(key=lambda item: item[0])
        removed = 0
        for _, size, path in entries:
            if total <= target:
                break
            if keep is not None and path == keep:
                continue
            try:
                path.unlink()
                total -= size
                removed += 1
            except OSError:
                continue
        return removed

    # ------------------------------------------------------------ 失效
    def invalidate_book(self, book_id: str) -> int:
        directory = self.dir_for(book_id)
        if directory is None:
            return 0  # 非法 id：不触碰任何路径（尤其不能 unlink）
        removed = self.memory.invalidate(lambda key: key[0] == book_id)
        try:
            if directory.is_dir():
                for path in directory.glob("*.json.gz"):
                    try:
                        path.unlink()
                        removed += 1
                    except OSError:
                        continue
                try:
                    directory.rmdir()
                except OSError:
                    pass
        except OSError:
            pass
        return removed

    def clear(self) -> None:
        self.memory.clear()
        try:
            for book_dir in self.dir.iterdir():
                if book_dir.is_dir():
                    for path in book_dir.glob("*.json.gz"):
                        try:
                            path.unlink()
                        except OSError:
                            continue
        except OSError:
            pass

    # ------------------------------------------------------------ 统计
    def stats(self) -> Dict[str, Any]:
        layouts = 0
        total = 0
        try:
            for book_dir in self.dir.iterdir():
                if not book_dir.is_dir():
                    continue
                for path in book_dir.glob("*.json.gz"):
                    layouts += 1
                    try:
                        total += path.stat().st_size
                    except OSError:
                        continue
        except OSError:
            pass
        return {"disk_layouts": layouts, "disk_bytes": total, "memory": self.memory.stats()}
