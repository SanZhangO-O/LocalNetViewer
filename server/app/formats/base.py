"""格式适配层基础接口与统一文档模型（设计 4.4 / 7.x）。

约定：
- 文本类格式（TXT/EPUB/FB2）解析为 ``DocumentModel``（章 → 块），mode=flow；
- 固定版式（PDF/漫画/文件夹图集）不生成 DocumentModel，直接以"页"为单位，
  mode=fixed，条漫切片为 mode=strip；
- 适配器实例是"一次性"的：由 registry 按请求创建，内部持有的句柄须在 close() 释放。
"""

from __future__ import annotations

import os
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from ..config import AppConfig
from ..util.paths import ext_of, stem_of

# ------------------------------------------------------------------ 块类型
BLOCK_HEADING = "heading"
BLOCK_PARAGRAPH = "paragraph"
BLOCK_IMAGE = "image"
BLOCK_SEPARATOR = "separator"

STYLE_NONE = "none"
STYLE_BOLD = "bold"
STYLE_ITALIC = "italic"
STYLE_BOLD_ITALIC = "bold_italic"


@dataclass
class Span:
    text: str
    style: str = STYLE_NONE

    def to_dict(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {"text": self.text}
        if self.style and self.style != STYLE_NONE:
            d["style"] = self.style
        return d

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "Span":
        return Span(text=str(d.get("text", "")), style=str(d.get("style", STYLE_NONE)))

    @property
    def bold(self) -> bool:
        return self.style in (STYLE_BOLD, STYLE_BOLD_ITALIC)

    @property
    def italic(self) -> bool:
        return self.style in (STYLE_ITALIC, STYLE_BOLD_ITALIC)


@dataclass
class Block:
    """统一块模型：heading / paragraph（spans）/ image / separator。"""

    type: str
    spans: List[Span] = field(default_factory=list)
    level: int = 1
    asset: str = ""
    w: int = 0
    h: int = 0

    # ------------------------------------------------------------ 便捷构造
    @staticmethod
    def paragraph(text: str) -> "Block":
        return Block(type=BLOCK_PARAGRAPH, spans=[Span(text)] if text else [])

    @staticmethod
    def heading(text: str, level: int = 1) -> "Block":
        return Block(type=BLOCK_HEADING, spans=[Span(text)], level=max(1, min(6, level)))

    @staticmethod
    def image(asset: str, w: int, h: int) -> "Block":
        return Block(type=BLOCK_IMAGE, asset=asset, w=int(w), h=int(h))

    @staticmethod
    def separator() -> "Block":
        return Block(type=BLOCK_SEPARATOR)

    # ------------------------------------------------------------ 属性
    @property
    def text(self) -> str:
        return "".join(s.text for s in self.spans)

    @property
    def is_text(self) -> bool:
        return self.type in (BLOCK_HEADING, BLOCK_PARAGRAPH)

    @property
    def is_empty(self) -> bool:
        if self.is_text:
            return not self.text.strip()
        return False

    def style_at(self, char_index: int) -> str:
        """返回块内第 char_index 个字符的样式（供 JSON 模式/调试用）。"""
        pos = 0
        for span in self.spans:
            n = len(span.text)
            if char_index < pos + n:
                return span.style
            pos += n
        return STYLE_NONE

    # ------------------------------------------------------------ 序列化
    def to_dict(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {"type": self.type}
        if self.type == BLOCK_HEADING:
            d["level"] = self.level
            d["spans"] = [s.to_dict() for s in self.spans]
        elif self.type == BLOCK_PARAGRAPH:
            d["spans"] = [s.to_dict() for s in self.spans]
        elif self.type == BLOCK_IMAGE:
            d["asset"] = self.asset
            d["w"] = self.w
            d["h"] = self.h
        return d

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "Block":
        t = str(d.get("type", BLOCK_PARAGRAPH))
        spans = [Span.from_dict(s) for s in d.get("spans", [])]
        if not spans and d.get("text"):
            # 兼容设计 4.4 文档示例里的简化写法 {"type":"heading","text":"…"}
            spans = [Span(str(d["text"]))]
        return Block(
            type=t,
            spans=spans,
            level=int(d.get("level", 1)),
            asset=str(d.get("asset", "")),
            w=int(d.get("w", 0) or 0),
            h=int(d.get("h", 0) or 0),
        )


@dataclass
class Chapter:
    index: int
    title: str
    blocks: List[Block] = field(default_factory=list)

    @property
    def total_chars(self) -> int:
        return sum(len(b.text) for b in self.blocks if b.is_text)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "index": self.index,
            "title": self.title,
            "blocks": [b.to_dict() for b in self.blocks],
        }

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "Chapter":
        return Chapter(
            index=int(d.get("index", 0)),
            title=str(d.get("title", "")),
            blocks=[Block.from_dict(b) for b in d.get("blocks", [])],
        )


@dataclass
class DocumentModel:
    """解析缓存内容（设计 4.4）。"""

    book_id: str
    format: str
    meta: Dict[str, Any] = field(default_factory=dict)
    chapters: List[Chapter] = field(default_factory=list)

    @property
    def total_chars(self) -> int:
        return sum(c.total_chars for c in self.chapters)

    @property
    def total_blocks(self) -> int:
        return sum(len(c.blocks) for c in self.chapters)

    def chapter(self, index: int) -> Optional[Chapter]:
        if 0 <= index < len(self.chapters):
            return self.chapters[index]
        return None

    def block(self, chapter_index: int, block_index: int) -> Optional[Block]:
        ch = self.chapter(chapter_index)
        if ch is None:
            return None
        if 0 <= block_index < len(ch.blocks):
            return ch.blocks[block_index]
        return None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "book_id": self.book_id,
            "format": self.format,
            "meta": self.meta,
            "chapters": [c.to_dict() for c in self.chapters],
        }

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "DocumentModel":
        return DocumentModel(
            book_id=str(d.get("book_id", "")),
            format=str(d.get("format", "")),
            meta=dict(d.get("meta", {})),
            chapters=[Chapter.from_dict(c) for c in d.get("chapters", [])],
        )


# ------------------------------------------------------------------ 书籍引用
@dataclass
class BookRef:
    """由书库服务给出的书籍引用（真实路径只在服务器内部使用）。"""

    book_id: str
    vpath: str
    path: Path
    format: str
    size: int = 0
    mtime: float = 0.0
    virtual: bool = False
    # 内容修订号（sha1(id:size:mtime)[:12]，跨实现契约）：用于 ETag 与缓存键
    revision: str = ""
    extra: Dict[str, Any] = field(default_factory=dict)

    @property
    def name(self) -> str:
        return self.vpath.rsplit("/", 1)[-1]

    @property
    def title_guess(self) -> str:
        return stem_of(self.name) if not self.virtual else self.name


@dataclass
class AdapterEnv:
    """适配器运行环境（避免适配器直接依赖 AppContext 造成循环导入）。"""

    config: AppConfig
    parsed_cache: Any = None  # ParsedCache，可空
    fonts: Any = None  # FontManager（生成文字封面需要），可空


@dataclass
class AdapterMeta:
    title: str
    format: str
    mode: str = "flow"
    author: Optional[str] = None
    language: Optional[str] = None
    size: int = 0
    page_count: Optional[int] = None
    chapters: List[Dict[str, Any]] = field(default_factory=list)
    supports_json: bool = True

    def to_meta_dict(self) -> Dict[str, Any]:
        return {
            "title": self.title,
            "author": self.author,
            "language": self.language,
            "size": self.size,
            "mode": self.mode,
            "page_count": self.page_count,
            "chapters": self.chapters,
            "supports_json": self.supports_json,
        }


class UnsupportedFormatError(Exception):
    """格式不支持（UNSUPPORTED_FORMAT），附带给客户端的原因说明。"""

    def __init__(self, message: str, *, reason: Optional[str] = None) -> None:
        super().__init__(message)
        self.reason = reason or message


class ParseError(Exception):
    """解析失败（PARSE_FAILED）。"""


class AssetNotFoundError(Exception):
    """书内资源不存在（ASSET_NOT_FOUND）。"""


class ParseTimeoutError(ParseError):
    """解析超时（PARSE_FAILED，附原因）。

    ``orphaned=True`` 表示超时发生时工作线程**仍在运行**、可能还在读取调用方
    持有的适配器：调用方必须跳过 ``close()``，把关闭交给 GC（工作线程释放
    最后一个引用后由 ``BookAdapter.__del__`` 兜底，且 close 必须可重复调用）。
    """

    def __init__(self, message: str, *, orphaned: bool = False) -> None:
        super().__init__(message)
        self.orphaned = bool(orphaned)


class ServerBusyError(ParseError):
    """服务器繁忙（并发解析槽占满 / 同一任务已在进行中）。

    语义上是"过载，请稍后重试"（SERVER_BUSY / 503），不是"这本书解析失败"；
    单独成类以便错误处理器区分两者（客户端不应把繁忙当成书坏了而缓存失败）。
    """


@dataclass(frozen=True)
class StripPage:
    """条漫切片页：源图索引 + 纵向偏移（设计 2.3 / 7.4）。

    对应锚点 ``{chapter: 0, block: image_index, char: y0}``。
    """

    image_index: int
    y0: int
    height: int
    is_first_of_image: bool = False
    is_last_of_image: bool = False


# ------------------------------------------------------------------ 适配器
class BookAdapter(ABC):
    """书籍适配器基类。"""

    format: str = ""
    mode: str = "flow"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        self.env = env
        self.ref = ref
        # 超时遗弃标记：工作线程仍在使用本适配器时，调用方不得 close()（见 mark_orphaned）
        self._orphaned = False
        self._closed = False

    # ------------------------------------------------------------- 生命周期
    def close(self) -> None:  # pragma: no cover - 默认无资源
        return None

    def __del__(self) -> None:
        # 兜底关闭：正常路径已 close（mark_closed）的跳过；被 mark_orphaned() 遗弃的
        # 适配器在工作线程释放后被 GC 收到这里。半构造对象（__init__ 中途抛错）也要容错。
        if getattr(self, "_closed", False):
            return
        try:
            self.close()
        except BaseException:  # noqa: BLE001 - 解释器退出/半构造对象都不能报错
            pass

    def mark_orphaned(self) -> None:
        """标记：超时工作线程仍可能使用本适配器，调用方应跳过 close()（交给 GC）。"""
        self._orphaned = True

    def mark_closed(self) -> None:
        """正常 close 成功后调用：GC 兜底不再重复 close（恰好关闭一次）。"""
        self._closed = True

    @property
    def orphaned(self) -> bool:
        return bool(getattr(self, "_orphaned", False))

    def __enter__(self) -> "BookAdapter":
        return self

    def __exit__(self, *exc: Any) -> None:
        try:
            self.close()
        finally:
            self._closed = True

    # ------------------------------------------------------------- 通用能力
    @abstractmethod
    def meta(self) -> AdapterMeta:
        """容器级元数据（meta 阶段必须快）。"""

    @property
    def supports_strip(self) -> bool:
        """是否支持条漫切片（仅漫画 / 图集容器，设计 7.4）。"""
        return False

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        """封面图字节（JPEG/PNG）。无封面返回 None，由调用方生成占位。"""
        return None

    @property
    def supports_json(self) -> bool:
        return self.mode == "flow"

    @property
    def page_count_estimated(self) -> Optional[int]:
        return None

    # ------------------------------------------------------- flow 模式能力
    def document(self) -> DocumentModel:
        raise UnsupportedFormatError(f"{self.format} 不支持文本分页")

    # ------------------------------------------------------ fixed 模式能力
    def page_count(self) -> int:
        raise UnsupportedFormatError(f"{self.format} 不是固定版式格式")

    def page_bytes(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        """返回 (图片字节, mime)。page 为 1 基页号。"""
        raise UnsupportedFormatError(f"{self.format} 不支持取页")

    def render_native(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        """与 page_bytes 相同，但忽略内存/字节上限（供 strip 切片使用）。"""
        return self.page_bytes(
            page, fit=fit, quality=quality, viewport_w=viewport_w, render_scale=render_scale
        )

    def page_native_size(self, page: int) -> Tuple[int, int]:
        """页面原始像素尺寸（条漫切片需要）。"""
        raise UnsupportedFormatError(f"{self.format} 无法取得页面尺寸")

    def page_aspect(self, page: int) -> float:
        try:
            w, h = self.page_native_size(page)
            return (h / w) if w else 0.0
        except UnsupportedFormatError:
            return 0.0

    def pair_bytes(
        self,
        page: int,
        *,
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
        rtl: bool = False,
    ) -> Tuple[bytes, str]:
        """双页跨页（M4）：相邻两页横向拼接。"""
        raise UnsupportedFormatError(f"{self.format} 不支持跨页拼接")

    # --------------------------------------------------------- 条漫切片
    def strip_pages(self, viewport_h: int, overlap: int = 0) -> List[StripPage]:
        """按视口高度把超高长图切成多页（mode=strip）。"""
        raise UnsupportedFormatError(f"{self.format} 不支持条漫切片")

    def strip_slice_bytes(
        self,
        sp: StripPage,
        *,
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
        fit: str = "viewport",
    ) -> Tuple[bytes, str]:
        """取某一片条漫切片（直接裁剪原图，不缩放）。"""
        raise UnsupportedFormatError(f"{self.format} 不支持条漫切片")

    # --------------------------------------------------------- 书内资源
    def asset(self, path: str) -> Tuple[bytes, str]:
        raise UnsupportedFormatError(f"{self.format} 不含内嵌资源")


class FlowAdapter(BookAdapter):
    mode = "flow"


class FixedAdapter(BookAdapter):
    mode = "fixed"

    @property
    def supports_json(self) -> bool:
        return False


# ------------------------------------------------------------------ 工具
def is_image_ext(ext: str, image_exts: Sequence[str]) -> bool:
    return (ext or "").lower() in set(image_exts)


def format_from_path(path: Path | str) -> str:
    ext = ext_of(str(path))
    return ext.lstrip(".")


def file_size(path: Path) -> int:
    try:
        return int(os.path.getsize(path))
    except OSError:
        return 0
