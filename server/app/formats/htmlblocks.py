"""HTML → 块模型转换（单一实现，M5）。

从 ``epub.py`` 抽出的公共转换器：EPUB 正文、独立 HTML、Markdown（先转 HTML）
共用同一套遍历/空白整理/图片解析，避免多处实现漂移。

调用方通过 ``resolve_image(tag, src)`` 回调决定图片怎么落入块模型（EPUB 用
容器内条目路径，HTML 用书所在目录的相对路径）；返回 ``None`` 表示该图片忽略。
"""

from __future__ import annotations

import re
from typing import Callable, List, Optional, Sequence

from .base import Block, Span, STYLE_BOLD, STYLE_BOLD_ITALIC, STYLE_ITALIC, STYLE_NONE

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

ImageResolver = Callable[[object, str], Optional[Block]]


def html_to_blocks(html: bytes, resolve_image: Optional[ImageResolver] = None) -> List[Block]:
    """HTML 字节 → 块模型（标题/段落/图片/分隔线）。"""
    soup = make_soup(html)
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
            spans = spans_of(tag)
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
            spans = spans_of(tag)
            text = _spans_text(spans)
            if text.strip():
                blocks.append(Block(type="paragraph", spans=spans))
            continue
        if name == "hr":
            blocks.append(Block.separator())
            continue
        if name in ("img", "image"):
            if resolve_image is None:
                continue
            src = image_src(tag)
            if not src:
                continue
            block = resolve_image(tag, src)
            if block is not None:
                blocks.append(block)
    return blocks


def spans_of(tag) -> List[Span]:
    """把一个块级元素的后代文本整理成带样式的 span 列表。"""
    spans: List[Span] = []
    for node in tag.descendants:
        node_name = getattr(node, "name", None)
        if node_name is None:
            text = _WS_RE.sub(" ", str(node))
            if not text:
                continue
            _append_span(spans, text, _style_of(node.parent, tag))
        elif node_name == "br":
            _append_span(spans, " ", _style_of(node.parent, tag))
        elif node_name == "img":
            _append_span(spans, " ", STYLE_NONE)
    _tidy_spans(spans)
    return spans


def image_src(tag) -> str:
    """提取图片地址（兼容 src / data-src / xlink:href / Clark 记法）。"""
    for key in ("src", "data-src"):
        val = tag.get(key)
        if val:
            return str(val)
    for key in ("xlink:href", "{http://www.w3.org/1999/xlink}href"):
        val = tag.get(key)
        if val:
            return str(val)
    attrs = getattr(tag, "attrs", None) or {}
    for key, value in attrs.items():
        if str(key).endswith("href") and value:
            if isinstance(value, (list, tuple)):
                return str(value[0]) if value else ""
            return str(value)
    return ""


def make_soup(html: bytes):
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


def _append_direct_text(blocks: List[Block], tag) -> None:
    """只取容器的**直接**文本子节点（不含块级子元素，那些由子元素自己输出）。"""
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
    return STYLE_NONE


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
