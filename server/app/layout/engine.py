"""分页引擎与 PageMap（设计 4.6 / 2.3）。

设计要点：
- 输入 DocumentModel + LayoutProfile，输出 PageMap（页码 ↔ 锚点 / 章节起始页）；
- ``layout_page(anchor)`` 从任意页首锚点**只重排本页内容**，因此
  ``build()``（全量分页）与逐页渲染走的是同一段代码，两者页码/行宽天然一致；
- 锚点 ``{chapter, block, char}`` 中的 char 是块内纯文本偏移，行首缩进的
  全角空格不计入偏移（避免缩进影响锚点稳定性）；
- 固定版式（fixed）与条漫（strip）不重排文本，直接按页枚举。
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Dict, Iterator, List, Optional, Sequence, Tuple

from ..formats.base import (
    BLOCK_IMAGE,
    BLOCK_SEPARATOR,
    DocumentModel,
    ParseError,
    StripPage,
)
from ..models.schemas import LayoutProfile
from .fonts import FontManager, is_wide_char

EPS = 0.01
MAX_PAGES = 500_000  # 防御性上限（异常输入不至于把服务器拖死）
# 单个块超过该长度时分段折行（防御"整本没有换行"的 TXT）
LONG_BLOCK_CHARS = 200_000
# 单个 token（一段不含空白/全角的连续文本）的最大长度。
# 没有这个上限时，"整本没有空格与换行"的 TXT 会产生一个上百万字符的 token，
# 而每折一行都要对它做一次 text_width（O(token)）与切片（O(token)）→ O(n²)。
MAX_TOKEN_CHARS = 1024
# 超过该长度的 token 用"提前退出"的方式量宽度：只要证明它放不下这一行就够了
TOKEN_MEASURE_EXACT_MAX = 256

# 行首禁止出现的字符（避头尾，P2）：命中时允许最多 8% 的溢出
_NO_LINE_START = set("。，、！？：；）」』】》〉”’…—·%!?,.:;)]}>")
# 标题字号相对正文字号的倍率
_HEADING_RATIO = {1: 1.8, 2: 1.5, 3: 1.3, 4: 1.15, 5: 1.05, 6: 1.0}
SEPARATOR_RATIO = 1.6

AnchorTuple = Tuple[int, int, int]


# ------------------------------------------------------------------ 条目
@dataclass
class TextItem:
    kind: str  # heading | paragraph
    text: str
    width: float
    height: float
    font_size: int
    bold: bool
    italic: bool
    chapter: int
    block: int
    char_start: int
    char_end: int
    level: int = 0
    space_after: float = 0.0
    y: float = 0.0

    @property
    def anchor(self) -> AnchorTuple:
        return (self.chapter, self.block, self.char_start)


@dataclass
class ImageItem:
    asset: str
    display_w: float
    display_h: float
    height: float
    chapter: int
    block: int
    space_after: float = 0.0
    y: float = 0.0

    @property
    def anchor(self) -> AnchorTuple:
        return (self.chapter, self.block, 0)


@dataclass
class SeparatorItem:
    width: float
    height: float
    chapter: int
    block: int
    space_after: float = 0.0
    y: float = 0.0

    @property
    def anchor(self) -> AnchorTuple:
        return (self.chapter, self.block, 0)


@dataclass
class ChapterMark:
    """章起始标记（不占高度，仅用于 chapter_new_page 切页）。"""

    chapter: int
    title: str
    height: float = 0.0
    space_after: float = 0.0

    @property
    def anchor(self) -> AnchorTuple:
        return (self.chapter, 0, 0)


Item = Any  # TextItem | ImageItem | SeparatorItem | ChapterMark


@dataclass
class LaidOutPage:
    anchor: AnchorTuple
    items: List[Item] = field(default_factory=list)
    next_anchor: Optional[AnchorTuple] = None
    chapter_index: int = 0
    chapter_title: str = ""
    first_line: str = ""

    @property
    def used_height(self) -> float:
        if not self.items:
            return 0.0
        last = self.items[-1]
        return float(last.y + last.height + getattr(last, "space_after", 0.0))


@dataclass
class PageRecord:
    page: int  # 1 基
    anchor: AnchorTuple
    chapter_index: int = 0
    chapter_title: str = ""
    first_line: str = ""
    image_index: int = -1  # strip 模式
    y0: int = 0
    height: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "page": self.page,
            "anchor": list(self.anchor),
            "chapter_index": self.chapter_index,
            "chapter_title": self.chapter_title,
            "first_line": self.first_line,
            "image_index": self.image_index,
            "y0": self.y0,
            "height": self.height,
        }

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "PageRecord":
        anchor = d.get("anchor") or [0, 0, 0]
        return PageRecord(
            page=int(d.get("page", 1)),
            anchor=(int(anchor[0]), int(anchor[1]), int(anchor[2])),
            chapter_index=int(d.get("chapter_index", 0)),
            chapter_title=str(d.get("chapter_title", "")),
            first_line=str(d.get("first_line", "")),
            image_index=int(d.get("image_index", -1)),
            y0=int(d.get("y0", 0)),
            height=int(d.get("height", 0)),
        )


@dataclass
class PageMap:
    """页码 ↔ 锚点 / 章节 的映射（落盘格式见 4.6）。"""

    book_id: str
    layout_id: str
    mode: str = "flow"
    total_pages: int = 0
    pages: List[PageRecord] = field(default_factory=list)
    chapters: List[Dict[str, Any]] = field(default_factory=list)
    end_anchor: AnchorTuple = (0, 0, 0)
    created_at: float = 0.0

    def record(self, page: int) -> Optional[PageRecord]:
        if 1 <= page <= len(self.pages):
            return self.pages[page - 1]
        return None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "book_id": self.book_id,
            "layout_id": self.layout_id,
            "mode": self.mode,
            "total_pages": self.total_pages,
            "end_anchor": list(self.end_anchor),
            "created_at": self.created_at,
            "chapters": self.chapters,
            "pages": [p.to_dict() for p in self.pages],
        }

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "PageMap":
        raw_end = d.get("end_anchor")
        pages = [PageRecord.from_dict(p) for p in d.get("pages", [])]
        if isinstance(raw_end, (list, tuple)) and len(raw_end) >= 3:
            end_anchor = (int(raw_end[0]), int(raw_end[1]), int(raw_end[2]))
        elif pages:
            # 缺少 end_anchor（旧格式/被裁剪）：按"最后一页之后"推导，
            # 否则 (0,0,0) 会让 page_of_anchor 认为任何锚点都已越界
            last = pages[-1].anchor
            end_anchor = (last[0], last[1] + 1, 0)
        else:
            end_anchor = (0, 0, 0)
        pm = PageMap(
            book_id=str(d.get("book_id", "")),
            layout_id=str(d.get("layout_id", "")),
            mode=str(d.get("mode", "flow")),
            total_pages=int(d.get("total_pages", len(pages))),
            pages=pages,
            chapters=list(d.get("chapters", [])),
            end_anchor=end_anchor,
            created_at=float(d.get("created_at", 0.0)),
        )
        if not pm.total_pages:
            pm.total_pages = len(pages)
        return pm


# ------------------------------------------------------------------ 文本引擎
class FlowLayoutEngine:
    """文本重排引擎（mode=flow）。"""

    def __init__(self, doc: DocumentModel, profile: LayoutProfile, fonts: FontManager) -> None:
        self.doc = doc
        self.profile = profile
        self.fonts = fonts
        self.avail_w = float(profile.avail_width)
        self.avail_h = float(profile.avail_height)

    # ------------------------------------------------------------ 基础量
    @property
    def family(self) -> str:
        return self.profile.font.family

    @property
    def base_size(self) -> int:
        return int(self.profile.font.size_px)

    @property
    def spacing(self) -> float:
        return float(self.profile.font.letter_spacing_px)

    @property
    def line_ratio(self) -> float:
        return float(self.profile.font.line_height)

    @property
    def indent_chars(self) -> int:
        return int(self.profile.paragraph.first_line_indent_chars)

    def heading_size(self, level: int) -> int:
        ratio = _HEADING_RATIO.get(max(1, min(6, level)), 1.0)
        return max(8, int(round(self.base_size * ratio)))

    def first_anchor(self) -> AnchorTuple:
        return (0, 0, 0)

    def doc_end_anchor(self) -> AnchorTuple:
        if not self.doc.chapters:
            return (0, 0, 0)
        last_ch = len(self.doc.chapters) - 1
        last_block = max(0, len(self.doc.chapters[last_ch].blocks) - 1)
        return (last_ch, last_block + 1, 0)

    # ------------------------------------------------------------ 条目流
    def iter_items(self, start: Optional[AnchorTuple] = None) -> Iterator[Item]:
        """从 start 锚点起惰性产出排版条目（文本行 / 图片 / 分隔线 / 章标记）。"""
        ch_start, blk_start, char_start = start or (0, 0, 0)
        for ci in range(max(0, ch_start), len(self.doc.chapters)):
            chapter = self.doc.chapters[ci]
            yield ChapterMark(chapter=ci, title=chapter.title or "")
            # 直接从锚点块起步：旧实现每页都从块 0 enumerate 再 continue 跳过，
            # 单章长文（几十万块）分页退化成 O(块数 × 页数)（设计 4.6 要求线性扫描）
            bi_start = max(0, blk_start) if ci == ch_start else 0
            for bi in range(bi_start, len(chapter.blocks)):
                block = chapter.blocks[bi]
                if block.type == BLOCK_IMAGE:
                    item = self._image_item(ci, bi, block)
                    if item is not None:
                        yield item
                    continue
                if block.type == BLOCK_SEPARATOR:
                    h = round(self.base_size * self.line_ratio * SEPARATOR_RATIO, 2)
                    yield SeparatorItem(
                        width=self.avail_w,
                        height=h,
                        chapter=ci,
                        block=bi,
                        space_after=float(self.profile.paragraph.spacing_px),
                    )
                    continue
                if not block.is_text:
                    continue
                text = block.text
                offset = char_start if (ci == ch_start and bi == blk_start) else 0
                if offset >= len(text):
                    continue
                yield from self._text_items(ci, bi, block, text, offset)

    def _text_items(self, ci: int, bi: int, block, text: str, offset: int) -> Iterator[TextItem]:
        is_heading = block.type == "heading"
        size = self.heading_size(block.level) if is_heading else self.base_size
        line_h = round(size * self.line_ratio, 2)
        # 段首缩进只在"从块开头排版"时给出：翻页后从块中间继续排版时不能再缩进，
        # 否则每页都会多出两个全角空格（设计 4.6 的 first_line_indent_chars）
        indent_chars = 0 if is_heading else (self.indent_chars if offset <= 0 else 0)
        bold = is_heading or any(s.bold for s in block.spans)
        italic = (not is_heading) and any(s.italic for s in block.spans)
        spacing_after = 0.0 if is_heading else float(self.profile.paragraph.spacing_px)
        kind = "heading" if is_heading else "paragraph"
        level = block.level if is_heading else 0

        # 逐行产出（不先算完整块的所有行）：整本没有换行的 TXT 里，一个块可能有
        # 几十万字符，而一页只放得下几十行。先把整段折行算完是 O(块长) 的工作量，
        # 逐页重复一遍就退化成 O(块长 × 页数)。
        stream = self._iter_block_lines(text, max(0, offset), size, indent_chars)
        try:
            pending = next(stream)
        except StopIteration:
            return

        def build(line: Tuple[str, int, int], space: float) -> TextItem:
            line_text, c_start, c_end = line
            return TextItem(
                kind=kind,
                text=line_text,
                width=self.fonts.text_width(line_text, self.family, size, self.spacing),
                height=line_h,
                font_size=size,
                bold=bold,
                italic=italic,
                chapter=ci,
                block=bi,
                char_start=c_start,
                char_end=c_end,
                level=level,
                space_after=space,
            )

        # 一行前瞻：只有"整块的最后一行"才带段后间距
        for nxt in stream:
            yield build(pending, 0.0)
            pending = nxt
        yield build(pending, spacing_after)

    def _iter_block_lines(
        self, text: str, offset: int, size: int, indent_chars: int
    ) -> Iterator[Tuple[str, int, int]]:
        """跨"超长块分段"的惰性折行流；偏移量还原为块内绝对位置。"""
        total = len(text)
        pos = max(0, offset)
        first_segment = True
        while pos < total:
            end = min(total, pos + LONG_BLOCK_CHARS)
            segment = text[pos:end]
            for line_text, c_start, c_end in self.iter_lines(
                segment,
                0,
                size,
                indent_chars if first_segment else 0,
                # 只有第一段才允许"整段是空白"时补一个空行占位；后续段若全是空白，
                # 补出来的会是一个零长度文本项（白占一行高度，还可能让分页停在那里）
                emit_empty=first_segment,
            ):
                yield line_text, c_start + pos, c_end + pos
            first_segment = False
            pos = end

    def _image_item(self, ci: int, bi: int, block) -> Optional[ImageItem]:
        w = float(block.w or 0)
        h = float(block.h or 0)
        if w <= 0 or h <= 0:
            w, h = 600.0, 800.0
        max_w = self.avail_w * float(self.profile.image.max_width_ratio)
        max_h = self.avail_h * float(self.profile.image.max_height_ratio)
        if w > max_w or h > max_h:
            ratio = min(max_w / w, max_h / h)
        else:
            ratio = 1.0
        display_w = max(1.0, w * ratio)
        display_h = max(1.0, h * ratio)
        # 图片显示高度不得超过一页可用高度，否则永远排不下
        if display_h > self.avail_h:
            shrink = self.avail_h / display_h
            display_h *= shrink
            display_w *= shrink
        return ImageItem(
            asset=block.asset,
            display_w=display_w,
            display_h=display_h,
            height=round(display_h, 2),
            chapter=ci,
            block=bi,
            space_after=float(self.profile.paragraph.spacing_px),
        )

    # ------------------------------------------------------------ 折行
    def break_lines(
        self, text: str, start: int, size: int, indent_chars: int = 0
    ) -> List[Tuple[str, int, int]]:
        """贪心折行；返回 [(行文本(含首行缩进), 块内起始偏移, 结束偏移)]。

        一次性算完整段，供测试与需要完整结果的调用方使用；分页路径请用
        :meth:`iter_lines`，它是同一套逻辑的惰性版本。
        """
        return list(self.iter_lines(text, start, size, indent_chars))

    def iter_lines(
        self, text: str, start: int, size: int, indent_chars: int = 0, *, emit_empty: bool = True
    ) -> Iterator[Tuple[str, int, int]]:
        """贪心折行的惰性版本：产出一行就可以立刻排版/绘制。

        分页时 `layout_page` 一页只取几十行就停下；如果先把整段折行算完，
        超长块（整本无换行的 TXT）会让每页都做一遍 O(段长) 的工作。

        ``emit_empty=False`` 时，整段没有任何可见内容就不产出占位空行
        （用于超长块的后续分段）。
        """
        limit_full = self.avail_w
        indent_px = float(indent_chars) * float(size) if indent_chars else 0.0
        produced = 0
        source = _iter_tokens(text, max(0, start))
        # 单槽推回缓冲：一行的"断点"落在某个 token 之前时，把它留到下一行用
        pending: Optional[Tuple[str, int, int]] = None

        def next_token() -> Optional[Tuple[str, int, int]]:
            nonlocal pending
            if pending is not None:
                token, pending = pending, None
                return token
            return next(source, None)

        def push_back(token: Tuple[str, int, int]) -> None:
            nonlocal pending
            pending = token

        first_line = True
        while True:
            first = next_token()
            if first is None:
                break
            limit = limit_full - (indent_px if first_line else 0.0)
            parts: List[str] = []
            width = 0.0
            line_start = first[1]
            line_end = line_start
            token: Optional[Tuple[str, int, int]] = first
            while token is not None:
                tok_text, ts, te = token
                if not parts and tok_text.strip() == "":
                    # 块的第一行保留原始起点作为锚点（保证页首锚点 ≤ 页内任何位置）；
                    # 续行则把锚点推到第一个非空白字符
                    if not first_line:
                        line_start = te
                    line_end = te
                    token = next_token()
                    continue
                tw, exact = self._token_width(tok_text, size, limit)
                if parts and width + tw > limit + EPS:
                    # 避头尾容忍度依赖**精确**宽度：长 token 的 tw 只是下界，
                    # 拿它比较会把几百字符的超长串整段塞进一行（排到页外）
                    if exact and tok_text and tok_text[0] in _NO_LINE_START and width + tw <= limit * 1.08:
                        pass  # 避头尾：允许 8% 溢出
                    else:
                        push_back(token)
                        break
                if not parts and tw > limit + EPS:
                    piece, taken, piece_w = self._hard_split(tok_text, ts, limit, size)
                    parts.append(piece)
                    width += piece_w
                    line_end = ts + taken
                    if taken < len(tok_text):
                        push_back((tok_text[taken:], ts + taken, te))
                    token = None
                    break
                parts.append(tok_text)
                width += tw
                line_end = te
                token = next_token()
            raw = "".join(parts).rstrip()
            prefix = "\u3000" * indent_chars if (first_line and indent_chars) else ""
            if not parts and token is None and pending is None:
                # 末尾只剩空白：不要为它产出一整行（会白白占一行高度）
                break
            produced += 1
            yield prefix + raw, line_start, line_end
            first_line = False
            # 终止性：每轮至少消费一个 token（`first` 一定被消费；唯一"推回"的两种
            # 情况——宽度溢出与硬断——都发生在 parts 非空之后，即已消费了 `first`），
            # 因此不需要额外的进展计数器。
        if not produced and emit_empty:
            yield "\u3000" * indent_chars, max(0, start), max(0, start)

    def _token_width(self, token: str, size: int, limit: float) -> Tuple[float, bool]:
        """token 宽度；返回 ``(宽度, 是否精确)``。

        短 token（绝大多数情况）走 :meth:`FontManager.text_width` 的精确缓存路径。
        长 token 只可能是"没有空格的超长串"，反正要字符级硬断；这里逐字符累加并在
        超过 ``limit`` 时立刻停下 —— 但**必须**把"这个值只是下界"告诉调用方：
        避头尾的 8% 容忍度依赖精确宽度，拿截断值去比较会把一个几百字符的超长 token
        整段放进一行（实测能排出 3 倍页宽的行）。
        """
        if len(token) <= TOKEN_MEASURE_EXACT_MAX:
            return self.fonts.text_width(token, self.family, size, self.spacing), True
        width = 0.0
        for ch in token:
            width += self.fonts.char_width(ch, self.family, size, self.spacing)
            if width > limit:
                return width, False
        return width, True

    def _hard_split(self, token: str, ts: int, limit: float, size: int) -> Tuple[str, int, float]:
        """超宽单词（英文/URL）做字符级强制断行（设计 4.6）。"""
        width = 0.0
        take = 0
        for ch in token:
            w = self.fonts.char_width(ch, self.family, size, self.spacing)
            if take > 0 and width + w > limit + EPS:
                break
            width += w
            take += 1
        take = max(1, min(take, len(token)))
        width = sum(
            self.fonts.char_width(ch, self.family, size, self.spacing) for ch in token[:take]
        )
        return token[:take], take, width

    # ------------------------------------------------------------ 单页
    def layout_page(self, start: Optional[AnchorTuple]) -> LaidOutPage:
        """从锚点开始排一页；返回该页条目与下一页起始锚点。"""
        page = LaidOutPage(anchor=start or self.first_anchor())
        used = 0.0
        pending_anchor: Optional[AnchorTuple] = None
        cur_chapter: Optional[int] = None
        for item in self.iter_items(start):
            if isinstance(item, ChapterMark):
                if (
                    self.profile.chapter_new_page
                    and page.items
                    and item.chapter != cur_chapter
                ):
                    pending_anchor = item.anchor
                    break
                if cur_chapter is None:
                    cur_chapter = item.chapter
                    page.chapter_index = item.chapter
                    page.chapter_title = item.title
                continue
            height = float(getattr(item, "height", 0.0)) + float(getattr(item, "space_after", 0.0))
            if page.items and used + height > self.avail_h + EPS:
                pending_anchor = item.anchor
                break
            if height > self.avail_h + EPS and page.items:
                # 单个条目高于整页（防御）：此时也应换页，避免死循环
                pending_anchor = item.anchor
                break
            item.y = used
            if cur_chapter is None:
                cur_chapter = item.chapter
                page.chapter_index = item.chapter
                chapter = self.doc.chapter(item.chapter)
                page.chapter_title = chapter.title if chapter else ""
            page.items.append(item)
            used += height

        if page.items:
            first = page.items[0]
            page.anchor = first.anchor
            if isinstance(first, TextItem):
                page.first_line = first.text
            # 章节信息以首个条目所属章节为准（章标记可能被合并到上一页）
            page.chapter_index = first.chapter
            chapter = self.doc.chapter(first.chapter)
            page.chapter_title = chapter.title if chapter else ""
        elif pending_anchor is None:
            page.anchor = self.doc_end_anchor()

        page.next_anchor = pending_anchor
        return page

    # ------------------------------------------------------------ 全量
    def build(self, layout_id: Optional[str] = None) -> PageMap:
        pages: List[PageRecord] = []
        chapter_starts: Dict[int, int] = {}
        anchor: Optional[AnchorTuple] = self.first_anchor()
        guard = 0
        last_next: Optional[AnchorTuple] = None
        while anchor is not None:
            guard += 1
            if guard > MAX_PAGES:
                raise ParseError(f"分页失败：页数超过上限（{MAX_PAGES}）")
            laid = self.layout_page(anchor)
            if laid.items:
                page_no = len(pages) + 1
                pages.append(
                    PageRecord(
                        page=page_no,
                        anchor=laid.anchor,
                        chapter_index=laid.chapter_index,
                        chapter_title=laid.chapter_title,
                        first_line=laid.first_line,
                    )
                )
                # 章起始页按"该章第一个条目落在哪一页"记录：
                # chapter_new_page=false 时一章可能从页中间开始，只看页首章节会漏记
                for item in laid.items:
                    chapter_starts.setdefault(item.chapter, page_no)
            nxt = laid.next_anchor
            if nxt is None:
                last_next = None
                break
            if nxt <= anchor:
                # 防御：锚点必须前进，否则结束（避免死循环）
                last_next = None
                break
            last_next = nxt
            anchor = nxt

        chapters = []
        for ch in self.doc.chapters:
            chapters.append(
                {
                    "index": ch.index,
                    "title": ch.title,
                    "total_chars": ch.total_chars,
                    "start_page": chapter_starts.get(ch.index),
                }
            )
        pm = PageMap(
            book_id=self.doc.book_id,
            layout_id=layout_id or self.profile.layout_id(),
            mode="flow",
            total_pages=len(pages),
            pages=pages,
            chapters=chapters,
            end_anchor=last_next or self.doc_end_anchor(),
            created_at=0.0,
        )
        return pm


def _tokenize(text: str, start: int) -> List[Tuple[str, int, int]]:
    """切分为"最小不可断单元"：CJK/全角单字，拉丁单词，空白串。

    一次性物化整个列表，供测试/需要完整结果的调用方使用；分页走
    :func:`_iter_tokens`（惰性），否则每页都要为"从锚点到文末"的全部文本建 token。
    """
    return list(_iter_tokens(text, start))


def _iter_tokens(text: str, start: int = 0) -> Iterator[Tuple[str, int, int]]:
    """惰性分词：只产出被真正消费到的 token。

    这是分页性能的关键：`layout_page` 一页只排几十行，如果每页都先把
    "锚点 → 文末"全部切成 token（CJK 是每字一个 token），整本分页就会退化成
    O(文本长度 × 页数)。
    """
    i = max(0, start)
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in " \t":
            j = i
            while j < n and text[j] in " \t":
                j += 1
            yield text[i:j], i, j
            i = j
            continue
        if is_wide_char(ch):
            yield ch, i, i + 1
            i += 1
            continue
        j = i
        # 按**绝对位置**分块，而不是"从 start 起每 MAX_TOKEN_CHARS 个字符"：
        # 后者会让"从块中间恢复排版"（翻页 / 重建）与"从头顺序排版"得到不同的
        # token 边界，进而折出不同的行 —— 分页与逐页渲染必须完全同源。
        chunk_end = i - (i % MAX_TOKEN_CHARS) + MAX_TOKEN_CHARS
        while j < n and j < chunk_end and text[j] not in " \t" and not is_wide_char(text[j]):
            j += 1
        yield text[i:j], i, j
        i = j


# ------------------------------------------------------------------ 固定版式
def build_fixed_pagemap(
    book_id: str, layout_id: str, page_count: int, mode: str = "fixed"
) -> PageMap:
    """PDF / 漫画：锚点 ``(0, 页序号, 0)``（设计 2.3）。"""
    pages = [
        PageRecord(
            page=i + 1,
            anchor=(0, i, 0),
            chapter_index=0,
            chapter_title="",
            first_line="",
        )
        for i in range(max(0, page_count))
    ]
    return PageMap(
        book_id=book_id,
        layout_id=layout_id,
        mode=mode,
        total_pages=len(pages),
        pages=pages,
        chapters=[],
        end_anchor=(0, max(0, page_count), 0),
    )


def build_strip_pagemap(
    book_id: str, layout_id: str, slices: Sequence[StripPage]
) -> PageMap:
    """条漫：锚点 ``(0, 源图索引, 纵向偏移)``，切片高度按视口（设计 3.6 / 7.4）。"""
    pages: List[PageRecord] = []
    for i, sp in enumerate(slices):
        pages.append(
            PageRecord(
                page=i + 1,
                anchor=(0, sp.image_index, sp.y0),
                chapter_index=0,
                chapter_title="",
                first_line="",
                image_index=sp.image_index,
                y0=sp.y0,
                height=sp.height,
            )
        )
    end_anchor: AnchorTuple = (0, 0, 0)
    if slices:
        last = slices[-1]
        end_anchor = (0, last.image_index, last.y0 + last.height)
    return PageMap(
        book_id=book_id,
        layout_id=layout_id,
        mode="strip",
        total_pages=len(pages),
        pages=pages,
        chapters=[],
        end_anchor=end_anchor,
    )


def estimated_page_capacity(profile: LayoutProfile) -> float:
    """粗略估计一页可容纳的行数（供日志/调试）。"""
    line_h = max(1.0, float(profile.font.size_px) * float(profile.font.line_height))
    return math.floor(float(profile.avail_height) / line_h)
