"""HTML 适配器（M5 Phase 1）。

- 单文件 HTML 直接解析为流式块模型；图片资源从 HTML 所在目录解析（防目录穿越）；
- HTML→块 的转换逻辑在 ``formats/htmlblocks.py``（与 EPUB 共用同一实现）；
- 忽略 CSS/脚本（与 EPUB 同一取舍：统一排版）。
"""

from __future__ import annotations

from pathlib import Path
from typing import List, Optional, Tuple
from urllib.parse import unquote

from ..util import imaging
from ..util.paths import is_within, stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    AssetNotFoundError,
    Block,
    BookRef,
    Chapter,
    DocumentModel,
    FlowAdapter,
    ParseError,
)
from .htmlblocks import html_to_blocks, make_soup


def html_title(html: bytes) -> str:
    soup = make_soup(html)
    if soup is None:
        return ""
    try:
        if soup.title is not None and soup.title.string:
            return " ".join(str(soup.title.string).split())
    except Exception:
        pass
    return ""


class HtmlAdapter(FlowAdapter):
    """单文件 HTML：流式阅读，图片资源引用同目录文件。"""

    format = "html"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "html").lower()
        self._raw: Optional[bytes] = None
        self._blocks_cache: Optional[List[Block]] = None
        self._chapters_cache: Optional[List[Chapter]] = None
        self._encoding = ""
        self._title_cache: Optional[str] = None

    # ------------------------------------------------------------ 读取
    def _load_html(self) -> bytes:
        if self._raw is not None:
            return self._raw
        max_mb = int(self.env.config.limits.max_book_mb or 0)
        if max_mb and (self.ref.size or 0) > max_mb * 1024 * 1024:
            raise ParseError(f"文件超过上限 {max_mb}MB")
        try:
            data = self.ref.path.read_bytes()
        except OSError as exc:
            raise ParseError(f"文件无法读取: {exc}") from exc
        from ..util.textutil import decode_text

        text, enc = decode_text(data, self.env.config.encoding_override(self.ref.vpath))
        self._encoding = enc
        self._raw = text.encode("utf-8", errors="replace")
        return self._raw

    def _blocks(self) -> List[Block]:
        if self._blocks_cache is None:
            self._blocks_cache = html_to_blocks(self._load_html(), self._image_block)
        return self._blocks_cache

    # ------------------------------------------------------------ 图片
    def _image_block(self, tag, src: str) -> Optional[Block]:
        resolved = self._resolve_image(str(src or ""))
        if resolved is None:
            return None
        asset, width, height = resolved
        return Block.image(asset, width, height)

    def _resolve_image(self, src: str) -> Optional[Tuple[str, int, int]]:
        src = src.strip()
        if not src or src.startswith(("data:", "http://", "https://", "//", "mailto:")):
            return None
        rel = unquote(src.split("#", 1)[0]).replace("\\", "/").strip()
        if not rel:
            return None
        base = self.ref.path.parent.resolve()
        candidate = (base / rel).resolve()
        if not is_within(candidate, base) or not candidate.is_file():
            return None
        try:
            asset_key = candidate.relative_to(base).as_posix()
        except ValueError:
            return None
        size = _probe_file_size(candidate)
        if size is None:
            size = (600, 800)
        return asset_key, size[0], size[1]

    def asset(self, path: str) -> Tuple[bytes, str]:
        rel = str(path).replace("\\", "/").strip().lstrip("/")
        if not rel or ".." in rel.split("/"):
            raise AssetNotFoundError(f"非法资源路径: {path}")
        base = self.ref.path.parent.resolve()
        candidate = (base / rel).resolve()
        if not is_within(candidate, base) or not candidate.is_file():
            raise AssetNotFoundError(f"资源不存在: {path}")
        limit = int(self.env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024
        try:
            if limit:
                with open(candidate, "rb") as fh:
                    data = fh.read(limit + 1)
                if len(data) > limit:
                    raise ParseError(f"资源超过上限（{limit // 1048576}MB）: {path}")
            else:
                data = candidate.read_bytes()
        except OSError as exc:
            raise ParseError(f"资源读取失败 {path}: {exc}") from exc
        from ..util.paths import ext_of

        return data, imaging.sniff_mime(data, ext_of(candidate.name))

    # ------------------------------------------------------------ 章节
    def _split_chapters(self) -> List[Chapter]:
        if self._chapters_cache is not None:
            return self._chapters_cache
        self._chapters_cache = split_chapters_by_headings(self._blocks(), stem_of(self.ref.name))
        return self._chapters_cache

    def _title(self) -> str:
        if self._title_cache is None:
            title = html_title(self._load_html()) or ""
            if not title:
                for block in self._blocks():
                    if block.type == "heading" and block.text.strip():
                        title = block.text.strip()
                        break
            self._title_cache = title or stem_of(self.ref.name)
        return self._title_cache

    # ------------------------------------------------------------ meta
    def meta(self) -> AdapterMeta:
        chapters = self._split_chapters()
        return AdapterMeta(
            title=self._title(),
            format=self.format,
            mode="flow",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=None,
            chapters=[
                {"index": c.index, "title": c.title, "total_chars": c.total_chars}
                for c in chapters
            ],
            supports_json=True,
        )

    def document(self) -> DocumentModel:
        chapters = self._split_chapters()
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": self._title(),
                "author": None,
                "language": None,
                "encoding": self._encoding,
                "total_chars": sum(c.total_chars for c in chapters),
                "source": self.ref.vpath,
            },
            chapters=chapters,
        )

    # ------------------------------------------------------------ 封面
    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        for block in self._blocks():
            if block.type == "image" and block.asset:
                try:
                    data, mime = self.asset(block.asset)
                except Exception:
                    continue
                if not width:
                    return data, mime
                try:
                    img = imaging.decode_for_display(
                        data, width, self.env.config.limits.max_source_pixels, ext=_ext(block.asset)
                    )
                    if img.width > width:
                        img = imaging.fit_width(img, width)
                    return imaging.encode_image(img, prefer_jpeg=True, quality=84)
                except Exception:
                    continue
        from ..render.raster import text_cover

        return text_cover(self._title(), None, width=width or 320, fonts=self.env.fonts)


# ------------------------------------------------------------------ 章节切分（与 officebook 共用）
def split_chapters_by_headings(blocks: List[Block], fallback: str) -> List[Chapter]:
    """按一级/二级标题切章（首标题前的正文归入 fallback 章）。"""
    chapters: List[Chapter] = []
    cur: List[Block] = []
    cur_title: Optional[str] = None

    def flush() -> None:
        nonlocal cur, cur_title
        if cur or cur_title is not None:
            chapters.append(Chapter(index=len(chapters), title=cur_title or fallback, blocks=cur))
        cur = []
        cur_title = None

    for block in blocks:
        if block.type == "heading" and block.level <= 2 and block.text.strip():
            flush()
            cur_title = block.text.strip()
            cur.append(block)
            continue
        cur.append(block)
    flush()
    if not chapters:
        chapters = [Chapter(index=0, title=fallback, blocks=[])]
    for i, ch in enumerate(chapters):
        ch.index = i
    return chapters


# ------------------------------------------------------------------ 辅助
def _ext(name: str) -> str:
    from ..util.paths import ext_of

    return ext_of(name)


def _probe_file_size(path: Path) -> Optional[Tuple[int, int]]:
    try:
        from PIL import Image  # noqa: PLC0415

        with Image.open(path) as img:
            return int(img.width), int(img.height)
    except Exception:
        return None
