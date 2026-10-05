"""MOBI / AZW3 适配器：依赖 Calibre 的 ``ebook-convert`` 转 EPUB 后缓存（设计 7.1，P4 可选）。

- 启动/打开时探测 ``ebook-convert``；缺失则明确报错（不支持的格式要在列表置灰）；
- 转换结果落在 ``cache/converted/{book_id}.{revision}.epub``，之后完全走 EPUB 解析路径；
- 解析超时由调用方（解析缓存）控制，这里的子进程超时取 ``parse_timeout_s × 4``；
- 漫画型（KF8 漫画转出的"图片即正文"EPUB）改走固定版式漫画管线
  （:class:`MobiComicAdapter`），每张图一页、fit=viewport 适应屏幕，
  否则图片会被流式排版塞进带边距的版心，四周全是空边（真实书籍反馈）。
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
    BookAdapter,
    BookRef,
    DocumentModel,
    FlowAdapter,
    ParseError,
    UnsupportedFormatError,
)
from .epub import EpubAdapter
from .imagebook import EpubComicAdapter, image_book_info, scan_image_book

CONVERT_TIMEOUT_FACTOR = 4.0

# ``cache/converted`` 的总量上限：转换产物是"一次性"缓存，不设上限会被
# 大量 MOBI 撑爆磁盘。按 mtime LRU 清理到 80%，与 parsed/layouts 缓存同一套策略。
CONVERTED_CACHE_LIMIT_BYTES = 2 * 1024 * 1024 * 1024

# 每本书一把锁：并发首次打开同一本 MOBI 时，第二个请求会等第一个转换完，
# 而不是同时往同一个目标文件写（互相读到半成品 EPUB）
_CONVERT_LOCKS: dict = {}
_CONVERT_LOCKS_GUARD = threading.Lock()

# 清理串行化：启动清理与转换后清理可能并发，避免同时删同一批文件
_SWEEP_LOCK = threading.Lock()


def sweep_converted_cache(
    directory: Path,
    limit_bytes: int = CONVERTED_CACHE_LIMIT_BYTES,
    *,
    keep: Optional[Path] = None,
) -> int:
    """按 mtime LRU 清理 ``cache/converted``，返回删除的文件数（线程安全）。

    启动时与每次转换成功后调用；``keep`` 指向刚写好的文件，即使配额紧张
    也不淘汰它（否则刚转完就被删，下一次打开又得重转）。
    """
    limit = int(limit_bytes or 0)
    if limit <= 0:
        return 0
    with _SWEEP_LOCK:
        entries = []
        total = 0
        try:
            files = [p for p in Path(directory).glob("*.epub") if p.is_file()]
        except OSError:
            return 0
        for path in files:
            try:
                st = path.stat()
            except OSError:
                continue
            entries.append((st.st_mtime, st.st_size, path))
            total += st.st_size
        if total <= limit:
            return 0
        target = int(limit * 0.8)
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
        # 文件名带内容修订号：内容变化后新 rev 指向新
        # 文件，失效时仍在途的旧转换（先写 .tmp 再 replace）永远不会被新 rev 采用，
        # 关闭"失效与转换竞态"导致的旧 EPUB 回灌。无 rev 时退回 book_id。
        rev = str(getattr(ref, "revision", "") or "")
        return out_dir / (f"{ref.book_id}.{rev}.epub" if rev else f"{ref.book_id}.epub")

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
            # 临时名必须仍以 .epub 结尾：ebook-convert 按输出文件的扩展名选择
            # 输出插件，传 ".tmp" 会直接 ValueError: No plugin to handle output format: tmp
            tmp = target.with_name(target.name + ".tmp.epub")
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
            # 转换产物受配额约束：超限时按 mtime LRU 淘汰旧书（保留刚转好的）
            sweep_converted_cache(target.parent, keep=target)

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


# ------------------------------------------------------------------ 漫画型 MOBI

# 检测与固定版式适配已抽到 formats/imagebook.py（EPUB 与 MOBI 共用）。
# 这两个别名保留给回归测试与旧调用点。
_scan_manga = scan_image_book
_manga_info = image_book_info


class MobiComicAdapter(EpubComicAdapter):
    """漫画型 MOBI/AZW3：转换产物是"图片即正文"的 EPUB，按固定版式漫画打开。

    复用漫画管线（fit=viewport/original、双页跨页、条漫切片、封面），
    与 CBZ 的阅读体验一致；页序 = spine 文档顺序。
    """

    format = "mobi"


def open_mobi_adapter(env: AdapterEnv, ref: BookRef) -> BookAdapter:
    """MOBI/AZW3 适配器工厂：先转换，再按"是否漫画"分流到固定版式或流式适配器。

    registry 按扩展名只能选类，而漫画与否要看过转换产物 —— 所以 MOBI 在这里
    特殊处理：先借 MobiAdapter 完成转换（命中缓存则零开销），检测后再决定返回
    哪个适配器实例。
    """
    flow = MobiAdapter(env, ref)  # 构造即完成转换（缓存命中则直接复用）
    info = _manga_info(flow._inner)
    if not info["manga"]:
        return flow
    eref = flow._inner.ref
    flow.close()
    return MobiComicAdapter(env, ref, eref, info)
