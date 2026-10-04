"""解析缓存：统一文档模型 DocumentModel 的落盘缓存（设计 3.4）。

- 键：``book_id + parser_version``（book_id 本身是内容指纹，文件变化即换键）；
- 位置：``cache/parsed/{book_id}.{parser_version}.json.gz``；
- 内存：额外保留少量热文档（``cache.doc_memory_max``），因为逐页渲染需要
  DocumentModel，每次都读盘会拖慢翻页；
- 淘汰：磁盘按 ``cache.parsed_max_mb`` 配额 LRU 淘汰（按 mtime 最旧优先）。
"""

from __future__ import annotations

import gzip
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, Optional

from ..formats.base import DocumentModel, ParseError
from ..util.concurrency import KeyedLocks, run_with_timeout
from ..util.paths import is_safe_id
from .lru import CountLRU

# 解析逻辑变更时递增，使旧缓存自然失效（设计 3.4）
PARSER_VERSION = "1"

# 读命中续 mtime 的冷却时间：磁盘 LRU 看"最近使用"，但每次读都 utime
# 会让每个页面请求都产生一次 inode 写入；60s 内的重复读不再 touch。
_TOUCH_COOLDOWN_S = 60.0


class ParsedCache:
    def __init__(self, config, max_docs: Optional[int] = None) -> None:
        self.config = config
        self.dir: Path = config.parsed_cache_dir
        self.max_docs = int(max_docs if max_docs is not None else config.cache.doc_memory_max)
        self.memory = CountLRU(max_items=self.max_docs)
        self._locks = KeyedLocks()
        self._disk_lock = threading.Lock()
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass

    # ------------------------------------------------------------ 路径
    def path_for(self, book_id: str) -> Optional[Path]:
        # book_id 直接拼进文件名：非法值（分隔符 / ".." / 盘符）返回 None
        if not is_safe_id(book_id):
            return None
        return self.dir / f"{book_id}.{PARSER_VERSION}.json.gz"

    def has_disk(self, book_id: str) -> bool:
        path = self.path_for(book_id)
        if path is None:
            return False
        try:
            return path.is_file()
        except OSError:
            return False

    # ------------------------------------------------------------ 读取
    def get_memory(self, book_id: str) -> Optional[DocumentModel]:
        return self.memory.get(book_id)

    def load_disk(self, book_id: str) -> Optional[DocumentModel]:
        path = self.path_for(book_id)
        if path is None:
            return None
        try:
            with gzip.open(path, "rt", encoding="utf-8") as fh:
                data = json.load(fh)
        except FileNotFoundError:
            return None
        except (OSError, ValueError, EOFError, gzip.BadGzipFile):
            # 缓存损坏（含 UnicodeDecodeError：它是 ValueError 子类）→ 删除后重新解析
            self._quarantine(path)
            return None
        try:
            model = DocumentModel.from_dict(data)
        except Exception:
            self._quarantine(path)
            return None
        # 校验身份：缓存文件名可能被误改/错放；内嵌 book_id 与请求不符时按未命中
        # 处理（与 LayoutStore.get 一致）——拿到别的书的 DocumentModel 比没有缓存更糟
        if model.book_id and model.book_id != book_id:
            self._quarantine(path)
            return None
        if not model.book_id:
            model.book_id = book_id
        self._touch(path)
        return model

    @staticmethod
    def _touch(path) -> None:
        """读命中即更新 mtime：磁盘 LRU 按"最近使用"而不是"最近写入"淘汰。

        否则热缓存（频繁读取但写入很早）会先于冷缓存被清掉。
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

    def get_or_parse(
        self,
        book_id: str,
        loader: Callable[[], DocumentModel],
        *,
        timeout_s: float = 60.0,
        what: str = "解析",
    ) -> DocumentModel:
        """内存 → 磁盘 → 实际解析（解析在锁内进行，避免并发重复计算）。"""
        cached = self.memory.get(book_id)
        if cached is not None:
            return cached

        with self._locks.acquire(("parse", book_id)):
            cached = self.memory.get(book_id)
            if cached is not None:
                return cached
            model = self.load_disk(book_id)
            if model is None:
                # key 让"上一次解析超时后仍在跑"的情况下直接失败，
                # 而不是把同一本书再解析一遍（重复占用 CPU 与内存）
                model = run_with_timeout(
                    loader, timeout_s, what, key=("parse", book_id)
                )
                if model is None:
                    raise ParseError(f"{what}失败：解析器返回空结果")
                self.save_disk(model)
            self.memory.put(book_id, model)
            return model

    # ------------------------------------------------------------ 写入
    def save_disk(self, model: DocumentModel) -> None:
        path = self.path_for(model.book_id)
        if path is None:
            return  # 非法 id：不落盘、不构造路径
        tmp = path.with_suffix(path.suffix + ".tmp")
        try:
            with self._disk_lock:
                path.parent.mkdir(parents=True, exist_ok=True)
                with gzip.open(tmp, "wt", encoding="utf-8") as fh:
                    json.dump(model.to_dict(), fh, ensure_ascii=False, separators=(",", ":"))
                os.replace(tmp, path)
        except OSError:
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
            return
        self.enforce_quota()

    def invalidate(self, book_id: str) -> None:
        self.memory.pop(book_id)
        path = self.path_for(book_id)
        if path is None:
            return  # 非法 id：不触碰任何路径（尤其不能 unlink）
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass

    def clear(self) -> None:
        self.memory.clear()
        for path in self._iter_files():
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass

    # ------------------------------------------------------------ 配额
    def _iter_files(self):
        try:
            return [p for p in self.dir.glob("*.json.gz") if p.is_file()]
        except OSError:
            return []

    def disk_stats(self) -> Dict[str, Any]:
        files = self._iter_files()
        total = 0
        for path in files:
            try:
                total += path.stat().st_size
            except OSError:
                continue
        return {
            "files": len(files),
            "bytes": total,
            "max_bytes": int(self.config.cache.parsed_max_mb) * 1024 * 1024,
            "memory_items": len(self.memory),
        }

    def enforce_quota(self) -> int:
        quota = int(self.config.cache.parsed_max_mb or 0) * 1024 * 1024
        if quota <= 0:
            return 0
        entries = []
        total = 0
        for path in self._iter_files():
            try:
                st = path.stat()
            except OSError:
                continue
            entries.append((st.st_mtime, st.st_size, path))
            total += st.st_size
        if total <= quota:
            return 0
        target = int(quota * 0.8)
        entries.sort(key=lambda e: e[0])
        removed = 0
        for _, size, path in entries:
            if total <= target:
                break
            try:
                path.unlink()
                total -= size
                removed += 1
            except OSError:
                continue
        return removed
