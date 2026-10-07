"""扩展名 → 适配器注册表（M5：由 ``formats.specs`` 单一事实源驱动）。

同时负责：
- 可打开格式判定（供书库列表设置 ``openable`` / ``reason``）；
- 解压/转换后端探测（RAR 需 unrar/bsdtar、7z 需 py7zr、Calibre/LibreOffice/DjVu、
  可选图像插件等）；
- 未知扩展名的内容嗅探兜底；
- 单本书文件大小上限的统一校验。
"""

from __future__ import annotations

import dataclasses
import threading
from typing import Dict, List, Mapping, Optional, Type

from ..util import imaging
from ..util.paths import ext_key_of
from . import detect as detect_mod
from . import specs as specs_mod
from .base import (
    AdapterEnv,
    BookAdapter,
    BookRef,
    FixedAdapter,
    ParseError,
    UnsupportedFormatError,
)
from .comics import ComicAdapter, rar_backend_tool
from .djvu import DjvuAdapter, djvu_tools
from .external import bsdtar_tool
from .fb2 import Fb2Adapter
from .fbz import open_fbz_adapter
from .fontbook import FontSampleAdapter
from .htmlbook import HtmlAdapter
from .imagebook import open_epub_adapter
from .markdownbook import MarkdownAdapter
from .mobi import calibre_available, open_mobi_adapter
from .multipage import MultiPageImageAdapter
from .officebook import (
    DocxAdapter,
    OdtAdapter,
    PptxAdapter,
    XlsxAdapter,
    open_legacy_adapter,
    soffice_path,
)
from .pdf import PdfAdapter
from .rtfbook import RtfAdapter
from .txt import CodeAdapter, TxtAdapter

GALLERY_FORMAT = "gallery"

# 后端探测缓存（进程内一次性）
_BACKEND_CACHE: Optional[Dict[str, Dict[str, object]]] = None
_BACKEND_LOCK = threading.Lock()

# 扩展名覆盖（library.ext_overrides）：扩展名 → 格式 id
_OVERRIDES: Dict[str, str] = {}
_OVERRIDES_LOCK = threading.Lock()

_EXT_INDEX: Dict[str, specs_mod.FormatSpec] = {}
for _spec in specs_mod.FORMAT_SPECS:
    for _ext in _spec.exts:
        _EXT_INDEX[_ext] = _spec


def configure_overrides(mapping: Optional[Mapping[str, object]]) -> None:
    """应用 ``library.ext_overrides``（扩展名 → 格式 id），幂等。"""
    normalized: Dict[str, str] = {}
    for key, value in dict(mapping or {}).items():
        ext = str(key or "").strip().lower()
        if ext and not ext.startswith("."):
            ext = "." + ext
        fmt = str(value or "").strip().lower()
        if ext and fmt:
            normalized[ext] = fmt
    global _OVERRIDES
    with _OVERRIDES_LOCK:
        _OVERRIDES = normalized


def _override_for(key: str) -> Optional[str]:
    with _OVERRIDES_LOCK:
        return _OVERRIDES.get(key)


def format_of(name_or_path: str) -> Optional[str]:
    """扩展名 → 格式标识（不可打开的扩展名返回 None）。"""
    key = ext_key_of(name_or_path)
    if not key:
        return None
    override = _override_for(key)
    if override:
        return override
    spec = _EXT_INDEX.get(key)
    return spec.id if spec is not None else None


def spec_of(fmt: str) -> Optional[specs_mod.FormatSpec]:
    return specs_mod.SPEC_BY_ID.get(str(fmt or "").lower())


def is_override_ext(ext: str) -> bool:
    """该扩展名是否被 ``library.ext_overrides`` 显式配置（不受 include_ext 限制）。"""
    e = (ext or "").lower()
    return bool(e) and bool(_override_for(e))


def format_for_ref(ref: BookRef) -> str:
    if ref.virtual and (ref.format == GALLERY_FORMAT or ref.path.is_dir()):
        return GALLERY_FORMAT
    return format_of(ref.name) or (ref.format or "")


def is_media_format(fmt: str) -> bool:
    spec = spec_of(fmt)
    return bool(spec and spec.kind in ("audio", "video"))


def is_indexable_format(fmt: str) -> bool:
    """是否作为"书"进入书库索引（媒体与不可打开格式不进）。"""
    spec = spec_of(fmt)
    if spec is None:
        # 覆盖/嗅探出来的格式（text 等）按可读处理
        return bool(fmt)
    return bool(spec.mode)


def is_browsable_format(fmt: str) -> bool:
    """是否支持压缩包内容浏览（/books/{id}/contents）。"""
    spec = spec_of(fmt)
    if spec is None:
        return False
    return spec.adapter == "comic" and spec.kind in ("comic", "archive")


def is_fixed_format(fmt: str) -> bool:
    spec = spec_of(fmt)
    if spec is not None:
        return spec.mode in ("fixed", "strip")
    return fmt == GALLERY_FORMAT


# ---------------------------------------------------------------- 后端探测
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
    soffice = soffice_path() or ""
    ddjvu, djvused = djvu_tools()
    djvu_ok = bool(ddjvu and djvused)
    if djvu_ok:
        djvu_hint = str(ddjvu)
    elif ddjvu:
        djvu_hint = "缺少 djvused（DjVuLibre）"
    else:
        djvu_hint = "需安装 DjVuLibre（ddjvu + djvused）"
    bsdtar = bsdtar_tool()
    return {
        "zip": {"available": True, "hint": "内置 zipfile"},
        "rar": {"available": rar_ok, "hint": rar_hint},
        "7z": {"available": seven_ok, "hint": seven_hint},
        "tar": {"available": True, "hint": "内置 tarfile"},
        "pdf": {"available": _module_available("pypdfium2"), "hint": "pypdfium2"},
        "epub": {"available": _module_available("bs4"), "hint": "BeautifulSoup4"},
        "calibre": {"available": bool(calibre), "hint": calibre or "需安装 Calibre（MOBI/AZW3/CHM/LIT 等）"},
        "libreoffice": {"available": bool(soffice), "hint": soffice or "需安装 LibreOffice（旧版 .doc/.xls/.ppt）"},
        "djvu": {"available": djvu_ok, "hint": djvu_hint},
        "bsdtar": {"available": bool(bsdtar), "hint": bsdtar or "需 bsdtar（libarchive；Windows 10+ 自带 tar.exe）"},
        "iso": {"available": _module_available("pycdlib"), "hint": "pycdlib（pip install pycdlib）"},
        "heif": {"available": imaging.image_backend_ready("heif"), "hint": "pillow-heif"},
        "jxl": {"available": imaging.image_backend_ready("jxl"), "hint": "pillow-jxl-plugin"},
        "svg": {"available": imaging.image_backend_ready("svg"), "hint": "svglib + reportlab"},
        "psd": {"available": imaging.image_backend_ready("psd"), "hint": "psd-tools"},
        "raw": {"available": imaging.image_backend_ready("raw"), "hint": "rawpy"},
    }


def _module_available(name: str) -> bool:
    try:
        __import__(name)
        return True
    except Exception:
        return False


def openable_reason(fmt: str) -> Optional[str]:
    """该格式当前是否可用；不可用时返回原因（列表置灰用）。"""
    spec = spec_of(fmt)
    if spec is None or not spec.backend:
        return None
    status = backend_status().get(spec.backend)
    if status and not status.get("available"):
        return str(status.get("hint") or f"{spec.backend} 不可用")
    return None


# ---------------------------------------------------------------- 能力清单
def format_infos() -> List[Dict[str, object]]:
    """能力清单（``/server/formats``）：客户端可据此做图标/置灰/入口判定。"""
    out: List[Dict[str, object]] = []
    for spec in specs_mod.FORMAT_SPECS:
        reason = openable_reason(spec.id)
        out.append(
            {
                "id": spec.id,
                "label": spec.label,
                "kind": spec.kind,
                "exts": list(spec.exts),
                "mode": spec.mode,
                "openable": reason is None,
                "reason": reason,
            }
        )
    out.append(
        {
            "id": GALLERY_FORMAT,
            "label": "图片 / 图集",
            "kind": "image",
            "exts": sorted(imaging.supported_image_exts()),
            "mode": "fixed",
            "openable": True,
            "reason": None,
        }
    )
    return out


# ---------------------------------------------------------------- 适配器工厂
def _adapter_class_for_spec(spec: specs_mod.FormatSpec) -> Optional[Type[BookAdapter]]:
    return {
        "txt": TxtAdapter,
        "code": CodeAdapter,
        "html": HtmlAdapter,
        "markdown": MarkdownAdapter,
        "rtf": RtfAdapter,
        "docx": DocxAdapter,
        "odt": OdtAdapter,
        "pptx": PptxAdapter,
        "xlsx": XlsxAdapter,
        "fb2": Fb2Adapter,
        "pdf": PdfAdapter,
        "multipage": MultiPageImageAdapter,
        "djvu": DjvuAdapter,
        "font": FontSampleAdapter,
    }.get(spec.adapter)


def _build_adapter(
    env: AdapterEnv, ref: BookRef, spec: Optional[specs_mod.FormatSpec], recursive: bool
) -> BookAdapter:
    if spec is None:
        raise UnsupportedFormatError(
            f"不支持的格式: {ref.format or ref.name}",
            reason=f"不支持的文件类型 {ref.format or ref.name}".strip(),
        )
    if spec.adapter == "comic":
        return ComicAdapter(env, ref, recursive=recursive)
    if spec.adapter == "epub":
        # EPUB：图片型（漫画）按固定版式漫画打开，文本型走流式（工厂内部分流）
        return open_epub_adapter(env, ref)
    if spec.adapter == "mobi":
        # MOBI/AZW3 及 Calibre 族群：转换后按漫画/文本分流（工厂内部决定）
        return open_mobi_adapter(env, ref)
    if spec.adapter == "fbz":
        return open_fbz_adapter(env, ref)
    if spec.adapter in ("office_legacy_doc", "office_legacy_xls", "office_legacy_ppt"):
        return open_legacy_adapter(env, ref, spec.adapter)
    cls = _adapter_class_for_spec(spec)
    if cls is None:
        raise UnsupportedFormatError(
            f"格式 {spec.id} 没有适配器", reason=f"格式 {spec.id} 尚未实现"
        )
    return cls(env, ref)


def _with_format(ref: BookRef, fmt: str) -> BookRef:
    return dataclasses.replace(ref, format=fmt)


def _sniff_adapter(env: AdapterEnv, ref: BookRef, recursive: bool) -> Optional[BookAdapter]:
    """扩展名未识别时的内容嗅探（文本 / 图片 / 常见文档容器）。"""
    detected = detect_mod.detect_file(ref.path)
    if detected.kind == "text":
        return TxtAdapter(env, _with_format(ref, "text"))
    if detected.kind == "image":
        return ComicAdapter(env, _with_format(ref, GALLERY_FORMAT), recursive=recursive)
    if detected.kind == "book":
        spec = specs_mod.SPEC_BY_ID.get(detected.format)
        if spec is None:
            return None
        reason = openable_reason(spec.id)
        if reason:
            raise UnsupportedFormatError(
                f"{spec.id} 暂不可用：{reason}", reason=reason
            )
        return _build_adapter(env, _with_format(ref, spec.id), spec, recursive)
    return None


def create_adapter(env: AdapterEnv, ref: BookRef, *, recursive: bool = False) -> BookAdapter:
    """创建适配器；统一做大小上限、后端可用性与嗅探兜底。"""
    # 压缩包内嵌套书（设计 7.6）：extra 携带解析好的链信息，由 archivebook
    # 解压/过滤后委托回常规适配器链。放在最前面：嵌套 ref 是虚拟的，
    # max_book_mb 与 format_for_ref 的目录判定都不适用于它。
    if (ref.extra or {}).get("archive_entry"):
        from .archivebook import create_archive_adapter  # noqa: PLC0415 - 延迟导入避免循环

        return create_archive_adapter(env, ref)
    max_mb = int(env.config.limits.max_book_mb or 0)
    if max_mb and not ref.virtual and ref.size > max_mb * 1024 * 1024:
        raise ParseError(
            f"文件超过单本上限 {max_mb}MB（{ref.size / 1048576:.1f}MB），已拒绝"
        )
    fmt = format_for_ref(ref)
    spec = spec_of(fmt)
    if spec is not None:
        reason = openable_reason(fmt)
        if reason:
            raise UnsupportedFormatError(f"{fmt} 暂不可用：{reason}", reason=reason)
        return _build_adapter(env, ref, spec, recursive)
    if fmt == GALLERY_FORMAT:
        return ComicAdapter(env, ref, recursive=recursive)
    if fmt == "text":
        # 嗅探出来的纯文本伪格式（列表/索引里记为 "text"）
        return TxtAdapter(env, ref)
    if spec is None and not ref.virtual and not fmt:
        sniffed = _sniff_adapter(env, ref, recursive)
        if sniffed is not None:
            return sniffed
    raise UnsupportedFormatError(
        f"不支持的格式: {fmt or ref.name}",
        reason=f"不支持的文件类型 {fmt or ''}".strip(),
    )


def is_fixed_adapter(adapter: BookAdapter) -> bool:
    return isinstance(adapter, FixedAdapter)
