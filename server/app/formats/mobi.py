"""MOBI / AZW3 适配器：依赖 Calibre 的 ``ebook-convert`` 转 EPUB 后缓存（设计 7.1，P4 可选）。

- 启动/打开时探测 ``ebook-convert``；缺失则明确报错（不支持的格式要在列表置灰）；
- 转换结果落在 ``cache/converted/{book_id}.epub``，之后完全走 EPUB 解析路径；
- 解析超时由调用方（解析缓存）控制，这里的子进程超时取 ``parse_timeout_s × 4``。
"""

from __future__ import annotations

import os
import shutil
import subprocess
import threading
from pathlib import Path
from typing import Optional, Tuple

from ..util.paths import makedirs_quiet
from .base import (
    AdapterEnv,
    AdapterMeta,
    BookRef,
    DocumentModel,
    FlowAdapter,
    ParseError,
    UnsupportedFormatError,
)
from .epub import EpubAdapter

CONVERT_TIMEOUT_FACTOR = 4.0

# 每本书一把锁：并发首次打开同一本 MOBI 时，第二个请求会等第一个转换完，
# 而不是同时往同一个目标文件写（互相读到半成品 EPUB）
_CONVERT_LOCKS: dict = {}
_CONVERT_LOCKS_GUARD = threading.Lock()


def _lock_for(key: str) -> threading.Lock:
    with _CONVERT_LOCKS_GUARD:
        lock = _CONVERT_LOCKS.get(key)
        if lock is None:
            lock = threading.Lock()
            _CONVERT_LOCKS[key] = lock
        return lock


def calibre_available() -> Optional[str]:
    return shutil.which("ebook-convert")


def _is_usable(path: Path) -> bool:
    try:
        return path.is_file() and path.stat().st_size > 0
    except OSError:
        return False


def _unlink_quiet(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


class MobiAdapter(FlowAdapter):
    format = "mobi"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        target = self._converted_path(env, ref)
        self._inner = EpubAdapter(env, self._epub_ref(ref, target))

    # ------------------------------------------------------------ 转换
    @staticmethod
    def _converted_path(env: AdapterEnv, ref: BookRef) -> Path:
        out_dir = env.config.cache_dir / "converted"
        makedirs_quiet(out_dir)
        return out_dir / f"{ref.book_id}.epub"

    def _epub_ref(self, ref: BookRef, target: Path) -> BookRef:
        if not _is_usable(target):
            self._convert(ref, target)
        return BookRef(
            book_id=ref.book_id,
            vpath=ref.vpath,
            path=target,
            format="epub",
            size=int(target.stat().st_size),
            mtime=target.stat().st_mtime,
            virtual=ref.virtual,
            extra=dict(ref.extra),
        )

    def _convert(self, ref: BookRef, target: Path) -> None:
        """把 MOBI 转成 EPUB 缓存文件。

        并发安全：同一本书的转换串行执行（每本书一把锁），并且**先写临时文件再
        ``os.replace``** —— 否则第二个请求可能在第一个请求写到一半时就把目标文件
        当成成品 EPUB 打开，解析出残缺内容。
        """
        with _lock_for(str(target)):
            if _is_usable(target):
                return  # 等锁期间别人已经转好了
            exe = calibre_available()
            if not exe:
                raise UnsupportedFormatError(
                    "MOBI/AZW3 需要 Calibre 的 ebook-convert 转换，请安装 Calibre 后重试",
                    reason="需安装 Calibre",
                )
            timeout = max(
                60.0, float(self.env.config.limits.parse_timeout_s) * CONVERT_TIMEOUT_FACTOR
            )
            makedirs_quiet(target.parent)
            tmp = target.with_name(target.name + ".tmp")
            try:
                proc = subprocess.run(
                    [exe, str(ref.path), str(tmp)],
                    capture_output=True,
                    timeout=timeout,
                )
            except subprocess.TimeoutExpired as exc:
                _unlink_quiet(tmp)
                raise ParseError(f"Calibre 转换超时（>{timeout:.0f}s）") from exc
            except OSError as exc:
                _unlink_quiet(tmp)
                raise ParseError(f"Calibre 转换无法执行: {exc}") from exc
            if proc.returncode != 0 or not _is_usable(tmp):
                detail = (proc.stderr or b"").decode("utf-8", errors="replace")[-500:]
                _unlink_quiet(tmp)
                raise ParseError(f"Calibre 转换失败（exit={proc.returncode}）: {detail}")
            try:
                os.replace(tmp, target)
            except OSError as exc:
                _unlink_quiet(tmp)
                raise ParseError(f"转换结果无法落盘: {exc}") from exc

    def close(self) -> None:
        self._inner.close()

    # ------------------------------------------------------------ 委托
    def meta(self) -> AdapterMeta:
        meta = self._inner.meta()
        meta.format = self.ref.format or self.format
        return meta

    def document(self) -> DocumentModel:
        doc = self._inner.document()
        doc.format = self.ref.format or self.format
        return doc

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        return self._inner.cover(width)

    def asset(self, path: str) -> Tuple[bytes, str]:
        return self._inner.asset(path)
