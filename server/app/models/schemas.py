"""Pydantic DTO：HTTP 契约（设计文档 5.x）与内部模型序列化结构。"""

from __future__ import annotations

import hashlib
import json
from enum import Enum
from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel, ConfigDict, Field, field_validator

Mode = Literal["flow", "fixed", "strip"]
PageFormat = Literal["png", "json"]
FitMode = Literal["viewport", "original"]


# --------------------------------------------------------------------- 错误码
class ErrorCode(str, Enum):
    BAD_REQUEST = "BAD_REQUEST"
    BAD_PATH = "BAD_PATH"
    PATH_TRAVERSAL = "PATH_TRAVERSAL"
    UNAUTHORIZED = "UNAUTHORIZED"
    BOOK_NOT_FOUND = "BOOK_NOT_FOUND"
    LAYOUT_NOT_FOUND = "LAYOUT_NOT_FOUND"
    PAGE_OUT_OF_RANGE = "PAGE_OUT_OF_RANGE"
    ASSET_NOT_FOUND = "ASSET_NOT_FOUND"
    UNSUPPORTED_FORMAT = "UNSUPPORTED_FORMAT"
    PARSE_FAILED = "PARSE_FAILED"
    SERVER_BUSY = "SERVER_BUSY"
    NOT_FOUND = "NOT_FOUND"
    INTERNAL = "INTERNAL"


ERROR_HTTP_STATUS: Dict[str, int] = {
    ErrorCode.BAD_REQUEST.value: 400,
    ErrorCode.BAD_PATH.value: 400,
    ErrorCode.PATH_TRAVERSAL.value: 403,
    ErrorCode.UNAUTHORIZED.value: 401,
    ErrorCode.BOOK_NOT_FOUND.value: 404,
    ErrorCode.LAYOUT_NOT_FOUND.value: 404,
    ErrorCode.PAGE_OUT_OF_RANGE.value: 404,
    ErrorCode.ASSET_NOT_FOUND.value: 404,
    ErrorCode.NOT_FOUND.value: 404,
    ErrorCode.UNSUPPORTED_FORMAT.value: 415,
    ErrorCode.PARSE_FAILED.value: 422,
    ErrorCode.SERVER_BUSY.value: 503,
    ErrorCode.INTERNAL.value: 500,
}


class ErrorBody(BaseModel):
    """统一错误模型（设计 5.1）。"""

    code: str
    message: str
    detail: Optional[Dict[str, Any]] = None


# ----------------------------------------------------------------- 服务信息
class LibraryInfo(BaseModel):
    name: str
    book_count: int = 0
    path: Optional[str] = None


class FeaturesInfo(BaseModel):
    """服务器能力表。

    字段在 Python 侧叫 ``json_mode`` 而不是 ``json``：``json`` 会遮蔽
    ``BaseModel.json()``（pydantic 会为此发 UserWarning，未来版本可能直接报错）。
    对外契约不变 —— 序列化按 alias 输出 ``"json"``。
    """

    model_config = ConfigDict(populate_by_name=True)

    png: bool = True
    json_mode: bool = Field(default=True, alias="json")
    search: bool = True
    gallery: bool = True
    fits: List[str] = Field(default_factory=lambda: ["viewport", "original"])
    strip: bool = True
    progress_sync: bool = True
    # 压缩包内容浏览（设计 7.6）：客户端据此显示"查看内容"入口
    archive_browse: bool = False
    # 高/宽 超过该值的长图可启用条漫 strip 模式（客户端据此提示）
    strip_min_aspect: float = 1.5


class ServerInfoResponse(BaseModel):
    name: str
    version: str
    api_version: str = "v1"
    auth_required: bool = False
    libraries: List[LibraryInfo] = Field(default_factory=list)
    features: FeaturesInfo = Field(default_factory=FeaturesInfo)
    server_time: str


# --------------------------------------------------------------------- 目录
class GalleryInfo(BaseModel):
    image_count: int = 0
    can_open_as_book: bool = False


class FileEntry(BaseModel):
    name: str
    type: Literal["dir", "book", "file", "image"]
    path: str
    format: Optional[str] = None
    book_id: Optional[str] = None
    size: Optional[int] = None
    mtime: Optional[str] = None
    cover_url: Optional[str] = None
    gallery: Optional[GalleryInfo] = None
    openable: bool = True
    reason: Optional[str] = None


class FilesResponse(BaseModel):
    path: str
    parent: Optional[str] = None
    gallery: Optional[GalleryInfo] = None
    entries: List[FileEntry] = Field(default_factory=list)
    total: int = 0
    limit: int = 1000
    offset: int = 0
    scanning: bool = False
    libraries: Optional[List[LibraryInfo]] = None


# ------------------------------------------------------------- 压缩包内容
class ArchiveSummary(BaseModel):
    """压缩包内容概览（设计 7.6）。"""

    entry_count: int = 0
    image_count: int = 0
    depth: int = 0
    max_depth: int = 2


class ArchiveEntryItem(BaseModel):
    name: str
    type: Literal["dir", "book", "file", "image"]
    path: str
    format: Optional[str] = None
    size: Optional[int] = None
    gallery: Optional[GalleryInfo] = None
    preview_url: Optional[str] = None
    openable: bool = True
    reason: Optional[str] = None


class ArchiveContentsResponse(BaseModel):
    book_id: str
    revision: str = ""
    entry: str = ""
    parent: Optional[str] = None
    archive: ArchiveSummary = Field(default_factory=ArchiveSummary)
    entries: List[ArchiveEntryItem] = Field(default_factory=list)
    total: int = 0
    limit: int = 500
    offset: int = 0


class OpenArchiveRequest(BaseModel):
    entry: str


class OpenArchiveResponse(BaseModel):
    book_id: str
    title: str
    format: str
    kind: str = "book"
    mode: Optional[Mode] = None
    page_count: Optional[int] = None
    cover_url: Optional[str] = None
    depth: int = 1


# --------------------------------------------------------------------- 元数据
class ChapterInfo(BaseModel):
    index: int
    title: str
    total_chars: Optional[int] = None
    start_page: Optional[int] = None


class RenderSupport(BaseModel):
    """本书支持的渲染格式（同 :class:`FeaturesInfo`，用 alias 避开 ``json`` 遮蔽）。"""

    model_config = ConfigDict(populate_by_name=True)

    png: bool = True
    json_mode: bool = Field(default=False, alias="json")


class BookMetaResponse(BaseModel):
    book_id: str
    revision: str = ""
    title: str
    author: Optional[str] = None
    format: str
    language: Optional[str] = None
    size: int = 0
    mode: Mode = "flow"
    page_count: Optional[int] = None
    cover_url: Optional[str] = None
    render_support: RenderSupport = Field(default_factory=RenderSupport)
    chapters: List[ChapterInfo] = Field(default_factory=list)
    source: Optional[str] = None
    # 压缩包书籍才有：内容概览（设计 7.6）
    archive: Optional[ArchiveSummary] = None


# ----------------------------------------------------------------- 排版参数
class Viewport(BaseModel):
    w: int = Field(ge=1)
    h: int = Field(ge=1)


class Margin(BaseModel):
    left: int = Field(default=0, ge=0)
    right: int = Field(default=0, ge=0)
    top: int = Field(default=0, ge=0)
    bottom: int = Field(default=0, ge=0)


class FontSpec(BaseModel):
    family: str = "auto"
    size_px: int = Field(default=52, ge=8, le=400)
    line_height: float = Field(default=1.6, ge=1.0, le=4.0)
    letter_spacing_px: float = Field(default=0.0, ge=-10.0, le=40.0)


class ParagraphSpec(BaseModel):
    spacing_px: float = Field(default=20.0, ge=0.0, le=400.0)
    first_line_indent_chars: int = Field(default=2, ge=0, le=8)


class ImageSpec(BaseModel):
    max_width_ratio: float = Field(default=1.0, gt=0.0, le=2.0)
    max_height_ratio: float = Field(default=1.0, gt=0.0, le=2.0)


class LayoutProfile(BaseModel):
    """客户端上报的排版参数，全部为物理像素（设计 4.6）。"""

    viewport: Viewport
    margin: Margin = Field(default_factory=Margin)
    font: FontSpec = Field(default_factory=FontSpec)
    paragraph: ParagraphSpec = Field(default_factory=ParagraphSpec)
    image: ImageSpec = Field(default_factory=ImageSpec)
    chapter_new_page: bool = True
    mode: Mode = "flow"
    # 条漫切片重叠像素（strip 模式专用，等价于 comic.strip_overlap_px）
    strip_overlap_px: int = Field(default=0, ge=0, le=2000)

    @property
    def avail_width(self) -> int:
        return max(1, self.viewport.w - self.margin.left - self.margin.right)

    @property
    def avail_height(self) -> int:
        return max(1, self.viewport.h - self.margin.top - self.margin.bottom)

    def geometry_dict(self) -> Dict[str, Any]:
        """参与 layout_id 的几何字段（theme / render_scale 不参与，设计 4.6）。"""
        return {
            "viewport": [self.viewport.w, self.viewport.h],
            "margin": [self.margin.left, self.margin.right, self.margin.top, self.margin.bottom],
            "font": [
                self.font.family,
                self.font.size_px,
                round(float(self.font.line_height), 4),
                round(float(self.font.letter_spacing_px), 4),
            ],
            "paragraph": [
                round(float(self.paragraph.spacing_px), 4),
                self.paragraph.first_line_indent_chars,
            ],
            "image": [
                round(float(self.image.max_width_ratio), 4),
                round(float(self.image.max_height_ratio), 4),
            ],
            "chapter_new_page": bool(self.chapter_new_page),
            "mode": self.mode,
            "strip_overlap_px": int(self.strip_overlap_px),
        }

    def layout_id(self) -> str:
        payload = json.dumps(
            self.geometry_dict(), ensure_ascii=False, sort_keys=True, separators=(",", ":")
        )
        digest = hashlib.sha1(payload.encode("utf-8")).hexdigest()[:12]
        return f"L{digest}"


class LayoutResponse(BaseModel):
    layout_id: str
    # 内容修订号：内容变化后 layout_id 不变但 revision 变化（客户端据此丢弃页面缓存）
    revision: str = ""
    mode: Mode
    total_pages: int
    chapters: List[ChapterInfo] = Field(default_factory=list)
    cache_hit: bool = False
    # 固定版式辅助信息
    strip: bool = False


# --------------------------------------------------------------------- 锚点
class Anchor(BaseModel):
    chapter: int = 0
    block: int = 0
    char: int = 0

    def key(self) -> tuple:
        return (self.chapter, self.block, self.char)

    def to_pair(self) -> tuple:
        return (self.chapter, self.block, self.char)


class PageMetaResponse(BaseModel):
    page: int
    total_pages: int
    anchor: Anchor
    chapter_index: int
    chapter_title: str = ""
    first_line: str = ""
    mode: Mode = "flow"
    # 固定版式：页面高/宽比（客户端据此决定"条漫/跨页"提示）
    aspect: Optional[float] = None


class LocateResponse(BaseModel):
    page: int
    total_pages: int
    ratio: float
    # 定位到的那一页所属章节（与 /pages/{n}/meta 的 chapter_index 一致）
    chapter_index: int
    chapter_title: str = ""
    # 锚点自身所属章节；chapter_new_page=false 时一页可含多章，两者可能不同
    anchor_chapter_index: Optional[int] = None
    clamped: bool = False


# --------------------------------------------------------------- JSON 页面模式
class JsonHeadingBlock(BaseModel):
    type: Literal["heading"] = "heading"
    level: int = 1
    lines: List[str] = Field(default_factory=list)


class JsonParagraphBlock(BaseModel):
    type: Literal["paragraph"] = "paragraph"
    lines: List[str] = Field(default_factory=list)


class JsonImageBlock(BaseModel):
    type: Literal["image"] = "image"
    asset: str
    display_w: float
    display_h: float


class JsonSeparatorBlock(BaseModel):
    type: Literal["separator"] = "separator"


class PageJsonResponse(BaseModel):
    page: int
    total_pages: int
    anchor: Anchor
    chapter_index: int
    chapter_title: str = ""
    blocks: List[Dict[str, Any]] = Field(default_factory=list)
    mode: Mode = "flow"


# --------------------------------------------------------------------- 搜索
class SearchItem(BaseModel):
    chapter_index: int
    chapter_title: str
    block: int
    char: int
    page: Optional[int] = None
    snippet: str


class SearchResponse(BaseModel):
    query: str
    total: int
    results: List[SearchItem] = Field(default_factory=list)
    limited: bool = False


# --------------------------------------------------------------- 文件夹图集
class OpenFolderRequest(BaseModel):
    path: str
    recursive: Optional[bool] = None


class OpenFolderResponse(BaseModel):
    book_id: str
    title: str
    format: str = "gallery"
    page_count: int
    cover_url: Optional[str] = None
    mode: Mode = "fixed"


# --------------------------------------------------------------------- 字体
class FontInfo(BaseModel):
    id: str
    name: str
    file_name: str
    default: bool = False
    builtin: bool = False


class FontsResponse(BaseModel):
    default: str
    fonts: List[FontInfo] = Field(default_factory=list)


# --------------------------------------------------------------------- 进度
class ProgressRecord(BaseModel):
    book_id: str
    device_id: Optional[str] = None
    anchor: Anchor
    page: Optional[int] = None
    ratio: Optional[float] = None
    updated_at: str


class ProgressPutRequest(BaseModel):
    anchor: Anchor
    page: Optional[int] = None
    ratio: Optional[float] = None

    @field_validator("ratio")
    @classmethod
    def _clamp(cls, v: Optional[float]) -> Optional[float]:
        if v is None:
            return None
        return min(1.0, max(0.0, float(v)))
