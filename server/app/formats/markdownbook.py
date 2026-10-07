"""Markdown 适配器（M5 Phase 1）：Markdown → HTML → 通用块模型。

优先用 markdown-it-py（若安装）；否则退化为内置轻量转换器（标题/列表/代码块/
引用/分隔线/粗斜体/链接/图片），保证零依赖可用。
"""

from __future__ import annotations

import html as _html
import re
from typing import List

from .base import AdapterEnv, BookRef
from .htmlbook import HtmlAdapter


def markdown_to_html(text: str) -> str:
    try:
        from markdown_it import MarkdownIt  # noqa: PLC0415

        md = MarkdownIt("commonmark", {"html": True, "linkify": False})
        try:
            md = md.enable(["table", "strikethrough"])
        except Exception:
            pass
        return md.render(text)
    except Exception:
        return _minimal_markdown(text)


class MarkdownAdapter(HtmlAdapter):
    format = "markdown"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "markdown").lower()

    def _load_html(self) -> bytes:
        if self._raw is not None:
            return self._raw
        from ..util.textutil import decode_text

        max_mb = int(self.env.config.limits.max_book_mb or 0)
        if max_mb and (self.ref.size or 0) > max_mb * 1024 * 1024:
            from .base import ParseError

            raise ParseError(f"文件超过上限 {max_mb}MB")
        try:
            data = self.ref.path.read_bytes()
        except OSError as exc:
            from .base import ParseError

            raise ParseError(f"文件无法读取: {exc}") from exc
        markdown, enc = decode_text(data, self.env.config.encoding_override(self.ref.vpath))
        self._encoding = enc
        self._raw = markdown_to_html(markdown).encode("utf-8", errors="replace")
        return self._raw


# ------------------------------------------------------------------ 内置回退转换器
_FENCE_RE = re.compile(r"^```")
_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
_HR_RE = re.compile(r"^\s*([-*_])(?:\s*\1){2,}\s*$")
_LIST_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$")
_QUOTE_RE = re.compile(r"^\s*>\s?(.*)$")
_IMG_RE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def _minimal_markdown(text: str) -> str:
    """轻量 Markdown → HTML（无外置依赖时的回退，够读即可）。"""
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: List[str] = []
    in_para: List[str] = []
    in_code = False
    in_ul = False
    in_ol = False

    def close_para() -> None:
        nonlocal in_para
        if in_para:
            out.append("<p>" + " ".join(in_para) + "</p>")
            in_para = []

    def close_lists() -> None:
        nonlocal in_ul, in_ol
        if in_ul:
            out.append("</ul>")
            in_ul = False
        if in_ol:
            out.append("</ol>")
            in_ol = False

    for raw in lines:
        if _FENCE_RE.match(raw.strip()):
            close_para()
            close_lists()
            if in_code:
                out.append("</pre>")
            else:
                out.append("<pre>")
            in_code = not in_code
            continue
        if in_code:
            out.append(_html.escape(raw))
            continue
        if not raw.strip():
            close_para()
            close_lists()
            continue
        heading = _HEADING_RE.match(raw)
        if heading:
            close_para()
            close_lists()
            level = len(heading.group(1))
            out.append(f"<h{level}>{_inline(heading.group(2))}</h{level}>")
            continue
        if _HR_RE.match(raw):
            close_para()
            close_lists()
            out.append("<hr/>")
            continue
        quote = _QUOTE_RE.match(raw)
        if quote:
            close_para()
            close_lists()
            out.append("<blockquote><p>" + _inline(quote.group(1)) + "</p></blockquote>")
            continue
        item = _LIST_RE.match(raw)
        if item:
            close_para()
            ordered = item.group(2)[0].isdigit()
            if ordered and not in_ol:
                close_lists()
                out.append("<ol>")
                in_ol = True
            elif not ordered and not in_ul:
                close_lists()
                out.append("<ul>")
                in_ul = True
            out.append("<li>" + _inline(item.group(3)) + "</li>")
            continue
        if raw.lstrip().startswith(("|", "    ")):
            # 表格/缩进块：按预格式文本，保留视觉结构
            close_para()
            close_lists()
            out.append("<pre>" + _html.escape(raw) + "</pre>")
            continue
        in_para.append(_inline(raw.strip()))
    close_para()
    close_lists()
    if in_code:
        out.append("</pre>")
    return "<html><body>" + "\n".join(out) + "</body></html>"


def _inline(text: str) -> str:
    escaped = _html.escape(text)
    escaped = _IMG_RE.sub(lambda m: f'<img src="{m.group(2)}" alt="{m.group(1)}"/>', escaped)
    escaped = _LINK_RE.sub(lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", escaped)
    escaped = re.sub(r"__([^_]+)__", r"<b>\1</b>", escaped)
    escaped = re.sub(r"\*([^*]+)\*", r"<i>\1</i>", escaped)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    return escaped
