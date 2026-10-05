"""扩展名 → 适配器注册表（设计 4.3 / 7.1）。

同时负责：
- 可打开格式判定（供书库列表设置 ``openable`` / ``reason``）；
- 解压后端探测（CBR 需要 unrar/bsdtar，CB7 需要 py7zr，MOBI 需要 Calibre）；
- 单本书文件大小上限的统一校验。
"""

from __future__ import annotations

import threading
from typing import Dict, List, Optional, Type

from ..util.paths import ext_of
from .base import (
    AdapterEnv,
    BookAdapter,
    BookRef,
    FixedAdapter,
    ParseError,
    UnsupportedFormatError,
)
from .comics import ComicAdapter, rar_backend_tool
from .epub import EpubAdapter
from .fb2 import Fb2Adapter
from .imagebook import open_epub_adapter
from .mobi import MobiAdapter, calibre_available, open_mobi_adapter
from .pdf import PdfAdapter
from .txt import TxtAdapter

TXT_EXTS = {".txt", ".text", ".log"}
EPUB_EXTS = {".epub"}
PDF_EXTS = {".pdf"}
FB2_EXTS = {".fb2"}
MOBI_EXTS = {".mobi", ".azw3", ".azw"}
COMIC_EXTS = {".cbz", ".zip", ".cbr", ".rar", ".cb7", ".7z", ".cbt", ".tar", ".cbz3"}
GALLERY_FORMAT = "gallery"

# 后端探测缓存（进程内一次性）
_BACKEND_CACHE: Optional[Dict[str, Dict[str, object]]] = None
_BACKEND_LOCK = threading.Lock()

_FORMAT_BY_EXT: Dict[str, str] = {}
for _ext in TXT_EXTS:
    _FORMAT_BY_EXT[_ext] = "txt"
for _ext in EPUB_EXTS:
    _FORMAT_BY_EXT[_ext] = "epub"
for _ext in PDF_EXTS:
    _FORMAT_BY_EXT[_ext] = "pdf"
for _ext in FB2_EXTS:
    _FORMAT_BY_EXT[_ext] = "fb2"
for _ext in MOBI_EXTS:
    _FORMAT_BY_EXT[_ext] = "mobi"
for _ext in COMIC_EXTS:
    _FORMAT_BY_EXT[_ext] = _ext.lstrip(".")


def format_of(name_or_path: str) -> Optional[str]:
    """扩展名 → 格式标识（不可打开的扩展名返回 None）。"""
    return _FORMAT_BY_EXT.get(ext_of(name_or_path))


def is_openable_ext(ext: str) -> bool:
    return (ext or "").lower() in _FORMAT_BY_EXT


def supported_exts() -> List[str]:
    return sorted(_FORMAT_BY_EXT.keys())


def format_for_ref(ref: BookRef) -> str:
    if ref.virtual and (ref.format == GALLERY_FORMAT or ref.path.is_dir()):
        return GALLERY_FORMAT
    return _FORMAT_BY_EXT.get(ext_of(ref.name), "") or ref.format or ""


def backend_status() -> Dict[str, Dict[str, object]]:
    """解压/转换后端探测结果（启动打印 + 列表置灰）。"""
    global _BACKEND_CACHE
    with _BACKEND_LOCK:
        if _BACKEND_CACHE is not None:
            return {name: dict(data) for name, data in _BACKEND_CACHE.items()}
        status = _probe_backends()
        _BACKEND_CACHE = status
        return {name: dict(data) for name, data in status.items()}


def _probe_backends() -> Dict[str, Dict[str, object]]:
    rar_ok = False
    rar_hint = "需安装 unrar 或 bsdtar"
    try:
        import rarfile  # noqa: PLC0415

        # rarfile 4.4+ 找不到工具时抛 RarCannotExec，而不是返回 None（见 comics.rar_backend_tool）
        tool = rar_backend_tool(rarfile)
        rar_ok = tool is not None
        if rar_ok:
            rar_hint = str(tool)
        else:
            rar_hint = "已安装 rarfile，但缺少 unrar / bsdtar 后端"
    except Exception:
        rar_hint = "缺少 rarfile（pip install rarfile）"
    seven_ok = False
    seven_hint = "缺少 py7zr"
    try:
        import py7zr  # noqa: PLC0415

        seven_ok = True
        seven_hint = f"py7zr {getattr(py7zr, '__version__', '?')}"
    except Exception:
        pass
    calibre = calibre_available()
    return {
        "zip": {"available": True, "hint": "内置 zipfile"},
        "rar": {"available": rar_ok, "hint": rar_hint},
        "7z": {"available": seven_ok, "hint": seven_hint},
        "tar": {"available": True, "hint": "内置 tarfile"},
        "pdf": {"available": _module_available("pypdfium2"), "hint": "pypdfium2"},
        "epub": {"available": _module_available("bs4"), "hint": "BeautifulSoup4"},
        "calibre": {"available": bool(calibre), "hint": calibre or "需安装 Calibre（MOBI/AZW3）"},
    }


def _module_available(name: str) -> bool:
    try:
        __import__(name)
        return True
    except Exception:
        return False


def openable_reason(fmt: str) -> Optional[str]:
    """该格式当前是否可用；不可用时返回原因（列表置灰用）。

    探测结果进程内缓存：``backend_status()`` 会跑 ``rarfile.tool_setup()``
    （可能启动子进程）与 ``shutil.which``，而目录列表的**每一个条目**都会问一次
    （``create_adapter`` + 列表的 openable 字段），不缓存等于每列一次目录就做几百次探测。
    """
    status = backend_status()
    if fmt in ("cbr", "rar") and not status["rar"]["available"]:
        return str(status["rar"]["hint"])
    if fmt in ("cb7", "7z") and not status["7z"]["available"]:
        return str(status["7z"]["hint"])
    if fmt in ("mobi", "azw3", "azw") and not status["calibre"]["available"]:
        return str(status["calibre"]["hint"])
    if fmt == "pdf" and not status["pdf"]["available"]:
        return str(status["pdf"]["hint"])
    return None


def adapter_class_for(ref: BookRef) -> Type[BookAdapter]:
    fmt = format_for_ref(ref)
    if fmt == GALLERY_FORMAT:
        return ComicAdapter
    ext = ext_of(ref.name)
    if ext in TXT_EXTS:
        return TxtAdapter
    if ext in EPUB_EXTS:
        return EpubAdapter
    if ext in PDF_EXTS:
        return PdfAdapter
    if ext in FB2_EXTS:
        return Fb2Adapter
    if ext in MOBI_EXTS:
        return MobiAdapter
    if ext in COMIC_EXTS:
        return ComicAdapter
    raise UnsupportedFormatError(
        f"不支持的格式: {ext or ref.name}", reason=f"不支持的文件类型 {ext or ''}".strip()
    )


def create_adapter(env: AdapterEnv, ref: BookRef, *, recursive: bool = False) -> BookAdapter:
    """创建适配器；统一做大小上限与后端可用性校验。"""
    max_mb = int(env.config.limits.max_book_mb or 0)
    if max_mb and not ref.virtual and ref.size > max_mb * 1024 * 1024:
        raise ParseError(
            f"文件超过单本上限 {max_mb}MB（{ref.size / 1048576:.1f}MB），已拒绝"
        )
    fmt = format_for_ref(ref)
    reason = openable_reason(fmt)
    if reason:
        raise UnsupportedFormatError(f"{fmt} 暂不可用：{reason}", reason=reason)
    cls = adapter_class_for(ref)
    if cls is ComicAdapter:
        return ComicAdapter(env, ref, recursive=recursive)
    if cls is EpubAdapter:
        # EPUB：图片型（漫画，每页一图）按固定版式漫画打开（铺满屏幕），
        # 文本型仍走流式排版 —— 分流由工厂内部检测决定
        return open_epub_adapter(env, ref)
    if cls is MobiAdapter:
        # MOBI/AZW3：漫画型（图片为主）转 EPUB 后按固定版式漫画打开，
        # 文本型仍走流式 EPUB —— 分流要看转换产物，由工厂内部决定
        return open_mobi_adapter(env, ref)
    return cls(env, ref)


def is_fixed_adapter(adapter: BookAdapter) -> bool:
    return isinstance(adapter, FixedAdapter)
