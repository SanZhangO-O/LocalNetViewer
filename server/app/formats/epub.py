"""EPUB 适配器（设计 7.3）。

- 主路径：ebooklib 读 spine / toc / DC 元数据；
- 兜底路径（ebooklib 缺失或其 API 变化 / 文件不合规）：zipfile + lxml 手写
  OPF / spine / nav 解析（设计风险 11 的对策）；
- 两条路径共用同一套 HTML→块模型转换与资源读取，保证结果一致；
- meta 阶段只做容器级解析（spine、toc、章标题），首次 layout 才全量解析正文；
- 忽略 CSS（有意为之的统一排版），丢弃 script/style/注释。
"""

from __future__ import annotations

import posixpath
import re
import zipfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple
from urllib.parse import unquote

from ..util import imaging
from ..util.paths import ext_of, stem_of
from ..util.xmlutil import find_local as _find_local, text_local as _text_local
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
    STYLE_BOLD,
    STYLE_BOLD_ITALIC,
    STYLE_ITALIC,
    STYLE_NONE,
)

_HEADING_LEVELS = {"h1": 1, "h2": 2, "h3": 3, "h4": 4, "h5": 5, "h6": 6}
_TEXT_CONTAINERS = {
    "p",
    "div",
    "li",
    "blockquote",
    "pre",
    "td",
    "th",
    "figcaption",
    "dd",
    "dt",
    "section",
    "article",
    "main",
    "aside",
    "header",
    "footer",
    "caption",
    "address",
}
_BLOCK_LEVEL = set(_TEXT_CONTAINERS) | set(_HEADING_LEVELS) | {"tr", "table", "ul", "ol", "dl", "figure"}
_SKIP_TAGS = {
    "script",
    "style",
    "head",
    "title",
    "nav",
    "noscript",
    "template",
    "iframe",
    "object",
    "video",
    "audio",
    "math",
}
_WS_RE = re.compile(r"[ \t\r\n\f\v]+")
_CJK_SPACE_RE = re.compile(
    r"(?<=[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef])"
    r"[ \t]+"
    r"(?=[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef])"
)
_COVER_NAME_RE = re.compile(r"(^|/)cover[^/]*\.(jpe?g|png|gif|webp|bmp)$", re.IGNORECASE)


@dataclass
class _EpubDoc:
    name: str
    title: str
    html: bytes


@dataclass
class _EpubModel:
    title: str = ""
    author: Optional[str] = None
    language: Optional[str] = None
    docs: List[_EpubDoc] = field(default_factory=list)
    image_names: List[str] = field(default_factory=list)
    cover_name: Optional[str] = None
    via: str = "zip"


class EpubAdapter(FlowAdapter):
    format = "epub"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        try:
            self._zip = zipfile.ZipFile(ref.path)
        except (OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"EPUB 容器无法打开: {exc}") from exc
        self._zip_names = set(self._zip.namelist())
        self._lower_map = {n.lower(): n for n in self._zip_names}
        self._model: Optional[_EpubModel] = None
        self._image_size_cache: Dict[str, Tuple[int, int]] = {}

    def close(self) -> None:
        try:
            self._zip.close()
        except Exception:
            pass

    # ------------------------------------------------------------ 容器读取
    def _read_entry(self, name: str) -> bytes:
        """读取容器内资源，**带单条目大小上限**。

        压缩包守卫（``limits.max_archive_entry_mb``）本来只覆盖漫画容器；
        EPUB 走的是同一个 zipfile，没有守卫就意味着一个声明/解压后 2GB 的
        条目可以直接把内存打爆（zip 炸弹）。这里按流式读取强制上限。
        """
        for cand in (
            name,
            unquote(name),
            name.lstrip("/"),
            posixpath.normpath(name),
        ):
            if cand in self._zip_names:
                return self._read_guarded(cand)
        low = self._lower_map.get(name.lower())
        if low is not None:
            return self._read_guarded(low)
        base = posixpath.basename(name).lower()
        for entry in self._zip_names:
            if posixpath.basename(entry).lower() == base:
                return self._read_guarded(entry)
        raise AssetNotFoundError(f"EPUB 内不存在资源: {name}")

    def _read_guarded(self, entry: str) -> bytes:
        limit = int(self.env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024
        with self._zip.open(entry) as fh:
            if not limit:
                return fh.read()
            data = fh.read(limit + 1)
        if len(data) > limit:
            raise ParseError(
                f"EPUB 条目超过上限（{limit // 1048576}MB）: {entry}"
            )
        return data

    def _resolve(self, base_item: str, href: str) -> str:
        """把相对 href 解析为容器内路径（去掉 fragment，规范化 ../）。"""
        href = unquote(str(href or "").strip())
        if not href or href.startswith("data:"):
            return ""
        href = href.split("#", 1)[0]
        if not href:
            return ""
        if href.startswith("/"):
            return posixpath.normpath(href.lstrip("/"))
        base_dir = posixpath.dirname(base_item)
        return posixpath.normpath(posixpath.join(base_dir, href))

    # ------------------------------------------------------------ 模型加载
    def model(self) -> _EpubModel:
        if self._model is not None:
            return self._model
        loaded = self._load_with_ebooklib()
        if loaded is None or not loaded.docs:
            loaded = self._load_with_zip()
        if loaded is None:
            raise ParseError("EPUB 结构无法解析（缺少 OPF / spine）")
        self._model = loaded
        return loaded

    def _load_with_ebooklib(self) -> Optional[_EpubModel]:
        try:
            import ebooklib  # noqa: PLC0415
            from ebooklib import epub  # noqa: PLC0415
        except Exception:
            return None
        try:
            book = epub.read_epub(str(self.ref.path), options={"ignore_ncx": True})
        except Exception:
            return None

        title = _first_meta(book, "title") or stem_of(self.ref.name)
        author = _first_meta(book, "creator")
        language = _first_meta(book, "language")

        toc_titles: Dict[str, str] = {}
        try:
            _flatten_toc(book.toc, toc_titles)
        except Exception:
            pass

        docs: List[_EpubDoc] = []
        try:
            spine = list(book.spine)
        except Exception:
            spine = []
        for entry in spine:
            idref = entry[0] if isinstance(entry, (tuple, list)) and entry else entry
            item = None
            try:
                item = book.get_item_with_id(str(idref))
            except Exception:
                item = None
            if item is None:
                continue
            media = str(getattr(item, "media_type", "") or "")
            if not media.startswith("application/xhtml"):
                continue
            try:
                name = str(item.get_name())
                html = bytes(item.get_content())
            except Exception:
                continue
            norm = posixpath.normpath(name)
            docs.append(_EpubDoc(name=norm, title=toc_titles.get(norm, ""), html=html))

        image_names = [
            n for n in self._zip_names if ext_of(n) in (".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp")
        ]
        model = _EpubModel(
            title=title,
            author=author,
            language=language,
            docs=docs,
            image_names=sorted(image_names),
            cover_name=_guess_cover(image_names),
            via=f"ebooklib/{getattr(ebooklib, 'VERSION', '?')}",
        )
        return model

    def _load_with_zip(self) -> Optional[_EpubModel]:
        from lxml import etree  # noqa: PLC0415

        try:
            container_xml = self._read_entry("META-INF/container.xml")
        except AssetNotFoundError:
            return None
        try:
            container = etree.fromstring(container_xml)
        except etree.XMLSyntaxError:
            return None
        rootfiles = _find_local(container, "rootfile")
        if not rootfiles:
            return None
        opf_path = posixpath.normpath(str(rootfiles[0].get("full-path") or ""))
        if not opf_path:
            return None
        try:
            opf = etree.fromstring(self._read_entry(opf_path))
        except (AssetNotFoundError, etree.XMLSyntaxError):
            return None

        title = _text_local(opf, "title") or stem_of(self.ref.name)
        author = _text_local(opf, "creator")
        language = _text_local(opf, "language")

        manifest: Dict[str, Dict[str, str]] = {}
        for item in _find_local(opf, "item"):
            iid = str(item.get("id") or "")
            if not iid:
                continue
            manifest[iid] = {
                "href": str(item.get("href") or ""),
                "media_type": str(item.get("media-type") or ""),
                "properties": str(item.get("properties") or ""),
            }

        docs: List[_EpubDoc] = []
        for itemref in _find_local(opf, "itemref"):
            iid = str(itemref.get("idref") or "")
            info = manifest.get(iid)
            if not info:
                continue
            if not str(info["media_type"]).startswith("application/xhtml"):
                continue
            name = posixpath.normpath(
                posixpath.join(posixpath.dirname(opf_path), unquote(info["href"]))
            )
            try:
                html = self._read_entry(name)
            except AssetNotFoundError:
                continue
            docs.append(_EpubDoc(name=name, title="", html=html))

        # 目录：EPUB3 nav 优先，其次 NCX
        toc_titles: Dict[str, str] = {}
        nav_item = next(
            (i for i in manifest.values() if "nav" in str(i.get("properties", "")).split()),
            None,
        )
        if nav_item:
            nav_name = posixpath.normpath(
                posixpath.join(posixpath.dirname(opf_path), unquote(nav_item["href"]))
            )
            try:
                toc_titles.update(_toc_from_nav(self._read_entry(nav_name), nav_name))
            except Exception:
                pass
        if not toc_titles:
            for info in manifest.values():
                if info["media_type"] == "application/x-dtbncx+xml":
                    ncx_name = posixpath.normpath(
                        posixpath.join(posixpath.dirname(opf_path), unquote(info["href"]))
                    )
                    try:
                        toc_titles.update(_toc_from_ncx(self._read_entry(ncx_name), ncx_name))
                    except Exception:
                        pass
                    break

        for doc in docs:
            doc.title = toc_titles.get(doc.name, "")

        image_names = [
            n for n in self._zip_names if ext_of(n) in (".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp")
        ]
        return _EpubModel(
            title=title,
            author=author,
            language=language,
            docs=docs,
            image_names=sorted(image_names),
            cover_name=_guess_cover(image_names),
            via="zip+opf",
        )

    # ------------------------------------------------------------ meta
    def meta(self) -> AdapterMeta:
        model = self.model()
        chapters = [
            {"index": i, "title": doc.title or stem_of(doc.name), "total_chars": None}
            for i, doc in enumerate(model.docs)
        ]
        return AdapterMeta(
            title=model.title or stem_of(self.ref.name),
            format=self.format,
            mode="flow",
            author=model.author,
            language=model.language,
            size=self.ref.size,
            page_count=None,
            chapters=chapters,
            supports_json=True,
        )

    # ------------------------------------------------------------ 解析
    def document(self) -> DocumentModel:
        model = self.model()
        chapters: List[Chapter] = []
        total_chars = 0
        for i, doc in enumerate(model.docs):
            blocks = self._html_to_blocks(doc.html, doc.name)
            title = doc.title
            if not title:
                for b in blocks:
                    if b.type == "heading" and b.text.strip():
                        title = b.text.strip()
                        break
            if not title:
                title = stem_of(doc.name)
            chapter = Chapter(index=i, title=title, blocks=blocks)
            total_chars += chapter.total_chars
            chapters.append(chapter)
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": model.title or stem_of(self.ref.name),
                "author": model.author,
                "language": model.language,
                "total_chars": total_chars,
                "source": self.ref.vpath,
                "parser": model.via,
            },
            chapters=chapters,
        )

    # ------------------------------------------------------------ HTML → 块
    def _html_to_blocks(self, html: bytes, item_name: str) -> List[Block]:
        soup = _soup(html)
        if soup is None:
            return []
        # 不能用 `soup.body or soup`：BeautifulSoup 的 Tag 真值同样来自"有没有子元素"，
        # 空的 <body></body> 会被判为假值而退回整个文档（与 lxml 是同一类陷阱）
        body = soup.body if soup.body is not None else soup
        blocks: List[Block] = []
        for tag in body.find_all(True):
            name = str(getattr(tag, "name", "") or "").lower()
            if name in _SKIP_TAGS or _in_skipped(tag, body):
                continue
            if name in _HEADING_LEVELS:
                if _has_block_descendant(tag):
                    _append_direct_text(blocks, tag)
                    continue
                spans = self._spans(tag)
                text = _spans_text(spans)
                if text.strip():
                    blocks.append(
                        Block(type="heading", spans=spans, level=_HEADING_LEVELS[name])
                    )
                continue
            if name in _TEXT_CONTAINERS:
                if _has_block_descendant(tag):
                    # 容器自己也可能有直接文本（<li>介绍<p>…</p></li>），不能整块丢掉
                    _append_direct_text(blocks, tag)
                    continue
                spans = self._spans(tag)
                text = _spans_text(spans)
                if text.strip():
                    blocks.append(Block(type="paragraph", spans=spans))
                continue
            if name == "hr":
                blocks.append(Block.separator())
                continue
            if name in ("img", "image"):
                block = self._image_block(tag, item_name)
                if block is not None:
                    blocks.append(block)
        return blocks

    def _image_block(self, tag, item_name: str) -> Optional[Block]:
        src = (
            tag.get("src")
            or tag.get("data-src")
            # SVG 包裹的插图：HTML 解析器给出 "xlink:href"，XML 解析器给出 Clark 记法
            or tag.get("xlink:href")
            or tag.get("{http://www.w3.org/1999/xlink}href")
            or _attr_by_suffix(tag, "href")
            or ""
        )
        src = str(src).strip()
        if not src:
            return None
        if src.startswith("data:"):
            # 内联 data: URI 无法通过 /asset 提供（asset 不接受空路径），
            # 与其留一个永久损坏的图片块，不如丢弃（封面另有专门路径）
            return None
        resolved = self._resolve(item_name, src)
        if not resolved:
            return None
        w, h = self._image_size(resolved)
        return Block.image(resolved, w, h)

    def _image_size(self, name: str) -> Tuple[int, int]:
        if name in self._image_size_cache:
            return self._image_size_cache[name]
        size = (600, 800)
        try:
            data = self._read_entry(name)
            probed = imaging.probe_size(data)
            if probed:
                size = probed
        except Exception:
            pass
        self._image_size_cache[name] = size
        return size

    def _spans(self, tag) -> List[Span]:
        spans: List[Span] = []
        for node in tag.descendants:
            node_name = getattr(node, "name", None)
            if node_name is None:
                text = _WS_RE.sub(" ", str(node))
                if not text:
                    continue
                style = _style_of(node.parent, tag)
                _append_span(spans, text, style)
            elif node_name == "br":
                _append_span(spans, " ", _style_of(node.parent, tag))
            elif node_name == "img":
                _append_span(spans, " ", STYLE_NONE)
        _tidy_spans(spans)
        return spans

    # ------------------------------------------------------------ 封面/资源
    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        try:
            model = self.model()
        except ParseError:
            model = None
        name = model.cover_name if model else None
        if name is None and model and model.image_names:
            name = model.image_names[0]
        if name:
            try:
                data = self._read_entry(name)
            except AssetNotFoundError:
                data = None
            if data:
                if not width:
                    return data, imaging.sniff_mime(data, ext_of(name))
                try:
                    img = imaging.decode_for_display(data, width, self.env.config.limits.max_source_pixels)
                    if img.width > width:
                        img = imaging.fit_width(img, width)
                    return imaging.encode_image(img, prefer_jpeg=True, quality=84)
                except Exception:
                    return data, imaging.sniff_mime(data, ext_of(name))
        from ..render.raster import text_cover

        title = model.title if model else stem_of(self.ref.name)
        author = model.author if model else None
        return text_cover(title, author, width=width or 320, fonts=self.env.fonts)

    def asset(self, path: str) -> Tuple[bytes, str]:
        norm = posixpath.normpath(str(path).replace("\\", "/").lstrip("/"))
        data = self._read_entry(norm)
        return data, imaging.sniff_mime(data, ext_of(norm))


# ------------------------------------------------------------------ 辅助
def _append_direct_text(blocks: List[Block], tag) -> None:
    """只取容器的**直接**文本子节点（不含块级子元素的文本，那些由子元素自己输出）。

    顺序上会排在该容器的块级子元素之前，但保证一句话都不会丢。
    """
    parts: List[str] = []
    for child in getattr(tag, "children", []):
        if getattr(child, "name", None) is not None:
            continue
        text = str(child)
        if text.strip():
            parts.append(text)
    if not parts:
        return
    joined = _WS_RE.sub(" ", " ".join(parts))
    joined = _CJK_SPACE_RE.sub("", joined).strip()
    if joined:
        blocks.append(Block(type="paragraph", spans=[Span(joined)]))


def _attr_by_suffix(tag, suffix: str) -> Optional[str]:
    """按属性名后缀查找（兼容 ``href`` / ``xlink:href`` / Clark 记法三种写法）。"""
    attrs = getattr(tag, "attrs", None)
    if not attrs:
        return None
    for key, value in attrs.items():
        key_str = str(key)
        if key_str == suffix or key_str.endswith(":" + suffix):
            if isinstance(value, (list, tuple)):
                return str(value[0]) if value else None
            return str(value) if value is not None else None
    return None


def _first_meta(book, key: str) -> Optional[str]:
    try:
        values = book.get_metadata("DC", key)
    except Exception:
        return None
    for entry in values or []:
        value = entry[0] if isinstance(entry, (tuple, list)) and entry else entry
        if value:
            return str(value).strip() or None
    return None


def _flatten_toc(items, out: Dict[str, str], depth: int = 0) -> None:
    if depth > 8 or not items:
        return
    for item in items:
        if isinstance(item, (tuple, list)) and len(item) == 2:
            section, children = item
            href = getattr(section, "href", None)
            title = getattr(section, "title", None)
            if href and title:
                out.setdefault(_norm_href(str(href)), str(title))
            _flatten_toc(children, out, depth + 1)
            continue
        href = getattr(item, "href", None)
        title = getattr(item, "title", None)
        if href and title:
            out.setdefault(_norm_href(str(href)), str(title))


def _norm_href(href: str) -> str:
    h = unquote(href).split("#", 1)[0]
    return posixpath.normpath(h.lstrip("/"))


def _toc_from_nav(html: bytes, nav_name: str) -> Dict[str, str]:
    soup = _soup(html)
    if soup is None:
        return {}
    out: Dict[str, str] = {}
    for tag in soup.find_all("a"):
        href = tag.get("href")
        text = tag.get_text(" ", strip=True)
        if not href or not text:
            continue
        target = posixpath.normpath(
            posixpath.join(posixpath.dirname(nav_name), unquote(str(href)).split("#", 1)[0])
        )
        out.setdefault(target, text)
    return out


def _toc_from_ncx(xml: bytes, ncx_name: str) -> Dict[str, str]:
    from lxml import etree  # noqa: PLC0415

    try:
        root = etree.fromstring(xml)
    except etree.XMLSyntaxError:
        return {}
    out: Dict[str, str] = {}
    for nav_point in _find_local(root, "navPoint"):
        label = _text_local(nav_point, "text")
        src = None
        for el in _find_local(nav_point, "content"):
            src = el.get("src")
            if src:
                break
        if not src:
            continue
        target = posixpath.normpath(
            posixpath.join(posixpath.dirname(ncx_name), unquote(str(src)).split("#", 1)[0])
        )
        if label:
            out.setdefault(target, label)
    return out


def _guess_cover(image_names: Sequence[str]) -> Optional[str]:
    if not image_names:
        return None
    for name in image_names:
        if _COVER_NAME_RE.search(name):
            return name
    for name in image_names:
        if "cover" in posixpath.basename(name).lower():
            return name
    return None


def _soup(html: bytes):
    try:
        from bs4 import BeautifulSoup  # noqa: PLC0415
    except Exception:
        return None
    for parser in ("lxml", "html.parser"):
        try:
            return BeautifulSoup(html, parser)
        except Exception:
            continue
    return None


def _in_skipped(tag, stop) -> bool:
    cur = getattr(tag, "parent", None)
    while cur is not None and cur is not stop:
        if getattr(cur, "name", None) in _SKIP_TAGS:
            return True
        cur = getattr(cur, "parent", None)
    return False


def _has_block_descendant(tag) -> bool:
    for child in tag.find_all(True):
        if getattr(child, "name", None) in _BLOCK_LEVEL:
            return True
    return False


def _style_of(node, stop) -> str:
    bold = italic = False
    cur = node
    while cur is not None and cur is not stop:
        name = getattr(cur, "name", None)
        if name in ("b", "strong"):
            bold = True
        elif name in ("i", "em"):
            italic = True
        cur = getattr(cur, "parent", None)
    if bold and italic:
        return STYLE_BOLD_ITALIC
    if bold:
        return STYLE_BOLD
    if italic:
        return STYLE_ITALIC
    return "none"


def _append_span(spans: List[Span], text: str, style: str) -> None:
    if not text:
        return
    if spans and spans[-1].style == style:
        spans[-1].text += text
    else:
        spans.append(Span(text, style))


def _tidy_spans(spans: List[Span]) -> None:
    for span in spans:
        span.text = _CJK_SPACE_RE.sub("", _WS_RE.sub(" ", span.text))
    if spans:
        spans[0].text = spans[0].text.lstrip(" ")
        spans[-1].text = spans[-1].text.rstrip(" ")
    # 合并后可能产生空 span
    cleaned = [s for s in spans if s.text]
    spans[:] = cleaned
    for i in range(len(spans) - 1):
        spans[i].text = spans[i].text.rstrip(" ")


def _spans_text(spans: Sequence[Span]) -> str:
    return "".join(s.text for s in spans)
