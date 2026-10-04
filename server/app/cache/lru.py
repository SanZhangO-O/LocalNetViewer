"""内存 LRU 容器。

- :class:`PageLRU`：页面图片缓存，**字节上限 + 张数上限双控**（设计 3.4 / 4.9）；
- :class:`CountLRU`：按对象数量淘汰的通用 LRU（热文档、PageMap）。
"""

from __future__ import annotations

import threading
from collections import OrderedDict
from typing import Any, Dict, Hashable, List, Optional


class PageLRU:
    """线程安全的字节 + 张数双上限 LRU。"""

    def __init__(self, max_bytes: int = 512 * 1024 * 1024, max_pages: int = 512) -> None:
        self.max_bytes = max(0, int(max_bytes))
        self.max_pages = max(0, int(max_pages))
        self._data: "OrderedDict[Hashable, Any]" = OrderedDict()
        self._sizes: Dict[Hashable, int] = {}
        self._bytes = 0
        self._lock = threading.RLock()
        self.hits = 0
        self.misses = 0

    # ------------------------------------------------------------ 基本操作
    def get(self, key: Hashable) -> Optional[Any]:
        with self._lock:
            if key not in self._data:
                self.misses += 1
                return None
            self._data.move_to_end(key)
            self.hits += 1
            return self._data[key]

    def put(self, key: Hashable, value: Any, size_bytes: Optional[int] = None) -> None:
        size = int(size_bytes if size_bytes is not None else 0)
        with self._lock:
            if self.max_bytes <= 0 and self.max_pages <= 0:
                # 两项上限都是 0 = 完全关闭页面缓存（不能解释成"无限缓存"）
                return
            # 单条超过字节上限的直接拒绝缓存：否则插入后 _evict 会为腾出空间
            # 逐出**全部**其他条目（最后连它自己也被逐出），一张 fit=original
            # 的大图就能让页面缓存在它的每次请求之间完全失效。
            if self.max_bytes and size > self.max_bytes:
                self.misses += 1
                return
            if key in self._data:
                self._bytes -= self._sizes.get(key, 0)
                self._data.pop(key, None)
            self._data[key] = value
            self._sizes[key] = size
            self._bytes += size
            self._evict()

    def _evict(self) -> None:
        while self._data:
            over_bytes = bool(self.max_bytes) and self._bytes > self.max_bytes
            over_pages = bool(self.max_pages) and len(self._data) > self.max_pages
            if not (over_bytes or over_pages):
                break
            key, _ = self._data.popitem(last=False)
            self._bytes -= self._sizes.pop(key, 0)

    def pop(self, key: Hashable) -> None:
        with self._lock:
            if key in self._data:
                self._data.pop(key, None)
                self._bytes -= self._sizes.pop(key, 0)

    def clear(self) -> None:
        with self._lock:
            self._data.clear()
            self._sizes.clear()
            self._bytes = 0

    def invalidate(self, predicate) -> int:
        """按前缀/条件清理（如清理某本书的旧 layout 页面缓存，设计 6.4）。"""
        removed = 0
        with self._lock:
            for key in [k for k in self._data.keys() if predicate(k)]:
                self._data.pop(key, None)
                self._bytes -= self._sizes.pop(key, 0)
                removed += 1
        return removed

    # ------------------------------------------------------------ 统计
    @property
    def size(self) -> int:
        with self._lock:
            return len(self._data)

    @property
    def bytes_used(self) -> int:
        with self._lock:
            return self._bytes

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            total = self.hits + self.misses
            return {
                "pages": len(self._data),
                "bytes": self._bytes,
                "max_bytes": self.max_bytes,
                "max_pages": self.max_pages,
                "hits": self.hits,
                "misses": self.misses,
                "hit_rate": round(self.hits / total, 4) if total else 0.0,
            }

    def keys(self) -> List[Hashable]:
        with self._lock:
            return list(self._data.keys())


class CountLRU:
    """按对象数量淘汰的 LRU（热文档 / PageMap）。"""

    def __init__(self, max_items: int = 8) -> None:
        self.max_items = max(1, int(max_items))
        self._data: "OrderedDict[Hashable, Any]" = OrderedDict()
        self._lock = threading.RLock()
        self.hits = 0
        self.misses = 0

    def get(self, key: Hashable) -> Optional[Any]:
        with self._lock:
            if key not in self._data:
                self.misses += 1
                return None
            self._data.move_to_end(key)
            self.hits += 1
            return self._data[key]

    def put(self, key: Hashable, value: Any) -> None:
        with self._lock:
            self._data[key] = value
            self._data.move_to_end(key)
            while len(self._data) > self.max_items:
                self._data.popitem(last=False)

    def pop(self, key: Hashable) -> None:
        with self._lock:
            self._data.pop(key, None)

    def invalidate(self, predicate) -> int:
        removed = 0
        with self._lock:
            for key in [k for k in self._data.keys() if predicate(k)]:
                self._data.pop(key, None)
                removed += 1
        return removed

    def clear(self) -> None:
        with self._lock:
            self._data.clear()

    def __contains__(self, key: Hashable) -> bool:
        with self._lock:
            return key in self._data

    def __len__(self) -> int:
        with self._lock:
            return len(self._data)

    def keys(self) -> List[Hashable]:
        with self._lock:
            return list(self._data.keys())

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            total = self.hits + self.misses
            return {
                "items": len(self._data),
                "max_items": self.max_items,
                "hits": self.hits,
                "misses": self.misses,
                "hit_rate": round(self.hits / total, 4) if total else 0.0,
            }
