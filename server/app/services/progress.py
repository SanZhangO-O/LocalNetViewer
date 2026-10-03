"""服务器端阅读进度同步（设计 5.2 #12，P4 可选，仅写 key-value）。

客户端默认本地存进度；开启该功能后可在多设备间同步。存储为
``cache/progress.json``，键为 ``{book_id}:{device_id}``。
"""

from __future__ import annotations

import json
import os
import threading
import time
from typing import Any, Dict, Optional

from ..util.paths import makedirs_quiet

# 进度记录上限：每条记录都是一个小字典，但 device_id 来自未校验的查询参数，
# 不限量的话 progress.json 可以被无限撑大（并且每次 PUT 都要整文件重写）。
MAX_RECORDS = 5000
MAX_DEVICE_ID_LEN = 64
# 落盘合并窗口：progress.json 每次都是整文件重写，连续翻页时不必每页写一次
SAVE_MIN_INTERVAL_S = 1.0


def normalize_device_id(device_id: Optional[str]) -> str:
    """规范化设备标识。

    未提供时用 ``default``（所有不传 device_id 的客户端共用一格，这是既有语义）；
    过长 / 含控制字符的值会被截断并剥离，避免用它拼 key 时撑大存储或写出畸形 JSON。
    """
    raw = str(device_id or "").strip()
    if not raw:
        return "default"
    cleaned = "".join(ch for ch in raw if ch.isprintable() and ch not in ':"\\/')
    cleaned = cleaned[:MAX_DEVICE_ID_LEN]
    return cleaned or "default"


class ProgressStore:
    def __init__(self, config) -> None:
        self.config = config
        self.path = config.cache_dir / "progress.json"
        self._lock = threading.RLock()
        self._data: Dict[str, Dict[str, Any]] = {}
        self._dirty = False
        self._last_save = 0.0
        self.load()

    def _key(self, book_id: str, device_id: Optional[str]) -> str:
        return f"{book_id}:{normalize_device_id(device_id)}"

    def load(self) -> None:
        try:
            raw = self.path.read_text(encoding="utf-8")
            data = json.loads(raw)
            if isinstance(data, dict):
                self._data = {str(k): dict(v) for k, v in data.items() if isinstance(v, dict)}
        except FileNotFoundError:
            self._data = {}
        except (OSError, json.JSONDecodeError, ValueError):
            self._data = {}

    def save(self, *, force: bool = False) -> bool:
        """把进度落盘。

        ``progress.json`` 是整文件重写，而连续翻页会不停 PUT；因此两次写入之间
        至少间隔 :data:`SAVE_MIN_INTERVAL_S`，期间只标记 dirty（与书库索引的
        ``maybe_save`` 同一套做法）。关停时用 ``force=True`` 或 :meth:`flush`。
        """
        with self._lock:
            now = time.time()
            if not force and (now - self._last_save) < SAVE_MIN_INTERVAL_S:
                self._dirty = True
                return False
            payload = json.dumps(self._data, ensure_ascii=False, indent=1)
            self._dirty = False
            self._last_save = now
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
            return False
        return True

    def flush(self) -> None:
        """强制落盘（进程关停时调用）。"""
        with self._lock:
            if not self._dirty:
                return
        self.save(force=True)

    def get(self, book_id: str, device_id: Optional[str] = None) -> Optional[Dict[str, Any]]:
        with self._lock:
            item = self._data.get(self._key(book_id, device_id))
            return dict(item) if item else None
    def get_any(self, book_id: str) -> Optional[Dict[str, Any]]:
        """未指定设备时返回该书最近一次进度。"""
        with self._lock:
            candidates = [
                v for k, v in self._data.items() if k.startswith(f"{book_id}:")
            ]
        if not candidates:
            return None
        candidates.sort(key=lambda v: float(v.get("updated_at_ts", 0.0)), reverse=True)
        return dict(candidates[0])

    def put(
        self,
        book_id: str,
        device_id: Optional[str],
        anchor: Dict[str, int],
        page: Optional[int] = None,
        ratio: Optional[float] = None,
    ) -> Dict[str, Any]:
        now = time.time()
        record = {
            "book_id": book_id,
            "device_id": normalize_device_id(device_id),
            "anchor": {
                "chapter": int(anchor.get("chapter", 0)),
                "block": int(anchor.get("block", 0)),
                "char": int(anchor.get("char", 0)),
            },
            "page": int(page) if page is not None else None,
            "ratio": float(ratio) if ratio is not None else None,
            "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
            "updated_at_ts": now,
        }
        with self._lock:
            self._data[self._key(book_id, device_id)] = record
            self._enforce_limit_locked()
        self.save()
        return record

    def _enforce_limit_locked(self) -> int:
        """按"最近更新"淘汰旧记录，保证 progress.json 不会无限增长（调用方需持锁）。"""
        overflow = len(self._data) - MAX_RECORDS
        if overflow <= 0:
            return 0
        ordered = sorted(
            self._data.items(),
            key=lambda item: float(item[1].get("updated_at_ts", 0.0)),
        )
        for key, _ in ordered[:overflow]:
            self._data.pop(key, None)
        return overflow

    def count(self) -> int:
        with self._lock:
            return len(self._data)
