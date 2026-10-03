"""FB2 适配器：XML → 块模型（设计 7.1，P3 顺带支持）。

FB2 结构简单：``description/title-info`` 提供元数据，``body/section`` 提供正文，
``binary`` 以 base64 内嵌图片。
"""

from __future__ import annotations

import base64
from typing import Dict, List, Optional, Tuple

from ..util import imaging
from ..util.paths import stem_of
from ..util.xmlutil import all_text, attr_local, find_local, first_local, localname, safe_fromstring
from lxml import etree  # noqa: PLC0415
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
    Span,
)

_RECURSE_TAGS = {"epigraph", "cite", "poem", "stanza", "annotation", "table", "tr"}
# 直接承载文本的元素（没有子元素）：必须自己输出，否则诗句/署名/日期/表格单元格会整段丢失
_TEXT_TAGS = {"v", "text-author", "date", "td", "th"}


class Fb2Adapter(FlowAdapter):
    format = "fb2"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self._root = None
        self._binaries: Dict[str, Tuple[bytes, str]] = {}
        self._loaded = False
        self._sizes: Dict[str, Tuple[int, int]] = {}

    # ------------------------------------------------------------ 解析
    def _load(self) -> None:
        if self._loaded:
            return

        max_mb = self.env.config.limits.max_book_mb
        if max_mb and (self.ref.size or 0) > max_mb * 1024 * 1024:
            raise ParseError(f"文件超过上限 {max_mb}MB")
        try:
            data = self.ref.path.read_bytes()
            self._root = safe_fromstring(data)
        except OSError as exc:
            raise ParseError(f"文件无法读取: {exc}") from exc
        except etree.XMLSyntaxError as exc:
            raise ParseError(f"FB2 XML 解析失败: {exc}") from exc

        for binary in find_local(self._root, "binary"):
            bid = str(binary.get("id") or "").strip()
            if not bid:
                continue
            ctype = str(binary.get("content-type") or "").strip() or "image/jpeg"
            payload = (binary.text or "").strip()
            if not payload:
                continue
            try:
                raw = base64.b64decode(payload, validate=False)
            except Exception:
                continue
            self._binaries[bid] = (raw, ctype)
        self._loaded = True

    # ------------------------------------------------------------ meta
    def _title_info(self):
        self._load()
        description = first_local(self._root, "description")
        if description is None:
            return None
        return first_local(description, "title-info")

    def _authors(self) -> Optional[str]:
        info = self._title_info()
        if info is None:
            return None
        names: List[str] = []
        for author in find_local(info, "author"):
            parts = [
                all_text(el).strip()
                for el in author
                if localname(el) in ("first-name", "middle-name", "last-name", "nickname")
            ]
            name = " ".join(p for p in parts if p).strip()
            if name:
                names.append(name)
        return ", ".join(names) or None

    def _sections(self) -> List:
        self._load()
        out: List = []
        for body in find_local(self._root, "body"):
            for section in body:
                if localname(section) == "section":
                    out.append(section)
            # body 直属的非 section 内容（罕见）归入一个匿名章
        return out

    def meta(self) -> AdapterMeta:
        info = self._title_info()
        title = ""
        language = None
        if info is not None:
            # 注意：**不能**写成 `first_local(info, "book-title") or info`，
            # 也不能把元素直接放进 if/三元条件 —— lxml 元素的真值来自"有没有子元素"，
            # `<book-title>示例</book-title>` 这种叶子节点是**假值**，会被当成"没找到"，
            # 于是书名退化成文件名。这里统一用 `is not None` 判断。
            title_el = first_local(info, "book-title")
            if title_el is not None:
                title = " ".join(all_text(title_el).split())
            lang_el = first_local(info, "lang")
            if lang_el is not None:
                language = all_text(lang_el).strip() or None
        title = title or stem_of(self.ref.name)
        chapters = [
            {"index": i, "title": _section_title(sec) or f"第 {i + 1} 节", "total_chars": None}
            for i, sec in enumerate(self._sections())
        ]
        return AdapterMeta(
            title=title,
            format=self.format,
            mode="flow",
            author=self._authors(),
            language=language,
            size=self.ref.size,
            page_count=None,
            chapters=chapters,
            supports_json=True,
        )

    def document(self) -> DocumentModel:
        info = self._title_info()
        title = stem_of(self.ref.name)
        language = None
        if info is not None:
            title_el = first_local(info, "book-title")
            if title_el is not None and all_text(title_el).strip():
                title = all_text(title_el).strip()
            lang_el = first_local(info, "lang")
            if lang_el is not None and all_text(lang_el).strip():
                language = all_text(lang_el).strip()

        chapters: List[Chapter] = []
        for i, section in enumerate(self._sections()):
            blocks = _section_blocks(section, level=1, adapter=self)
            chapters.append(
                Chapter(index=i, title=_section_title(section) or f"第 {i + 1} 节", blocks=blocks)
            )
        if not chapters:
            chapters = [Chapter(index=0, title=title, blocks=[])]
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": title,
                "author": self._authors(),
                "language": language,
                "total_chars": sum(c.total_chars for c in chapters),
                "source": self.ref.vpath,
            },
            chapters=chapters,
        )

    # ------------------------------------------------------------ 图片
    def _image_block(self, el) -> Optional[Block]:
        href = attr_local(el, "href") or attr_local(el, "src") or ""
        href = str(href).lstrip("#").strip()
        if not href:
            return None
        entry = self._binaries.get(href)
        if entry is None:
            return None
        raw = entry[0]
        size = self._sizes.get(href)
        if size is None:
            probed = imaging.probe_size(raw)
            size = probed or (600, 800)
            self._sizes[href] = size
        return Block.image(href, size[0], size[1])

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        info = self._title_info()
        name = None
        if info is not None:
            coverpage = first_local(info, "coverpage")
            if coverpage is not None:
                for el in coverpage.iter():
                    if localname(el) == "image":
                        href = attr_local(el, "href")
                        if href:
                            name = str(href).lstrip("#")
                            break
        if name is None and self._binaries:
            name = next(iter(self._binaries))
        if name and name in self._binaries:
            raw, ctype = self._binaries[name]
            if not width:
                return raw, ctype
            try:
                img = imaging.decode_for_display(raw, width, self.env.config.limits.max_source_pixels)
                if img.width > width:
                    img = imaging.fit_width(img, width)
                return imaging.encode_image(img, prefer_jpeg=True, quality=84)
            except Exception:
                return raw, ctype
        from ..render.raster import text_cover

        meta = self.meta()
        return text_cover(meta.title, meta.author, width=width or 320, fonts=self.env.fonts)

    def asset(self, path: str) -> Tuple[bytes, str]:
        key = str(path).lstrip("#")
        entry = self._binaries.get(key)
        if entry is None:
            for bid, val in self._binaries.items():
                if bid.lower() == key.lower():
                    entry = val
                    break
        if entry is None:
            raise AssetNotFoundError(f"FB2 内不存在资源: {path}")
        return entry


# ------------------------------------------------------------------ 辅助
def _section_title(section) -> str:
    for child in section:
        if localname(child) == "title":
            text = all_text(child).strip()
            if text:
                return " ".join(text.split())
    return ""


def _section_blocks(section, level: int, adapter: Fb2Adapter) -> List[Block]:
    blocks: List[Block] = []
    for child in section:
        tag = localname(child)
        if tag == "title":
            text = " ".join(all_text(child).split())
            if text:
                blocks.append(
                    Block(type="heading", spans=[Span(text)], level=max(1, min(6, level)))
                )
            continue
        if tag in ("subtitle",):
            text = " ".join(all_text(child).split())
            if text:
                blocks.append(
                    Block(type="heading", spans=[Span(text)], level=max(1, min(6, level + 1)))
                )
            continue
        if tag == "p":
            text = " ".join(all_text(child).split())
            if text:
                blocks.append(Block.paragraph(text))
            continue
        if tag in _TEXT_TAGS:
            text = " ".join(all_text(child).split())
            if text:
                blocks.append(Block.paragraph(text))
            continue
        if tag == "empty-line":
            blocks.append(Block.separator())
            continue
        if tag == "image":
            block = adapter._image_block(child)
            if block is not None:
                blocks.append(block)
            continue
        if tag == "section":
            blocks.extend(_section_blocks(child, level + 1, adapter))
            continue
        if tag in _RECURSE_TAGS:
            blocks.extend(_section_blocks(child, level, adapter))
            continue
    return blocks
