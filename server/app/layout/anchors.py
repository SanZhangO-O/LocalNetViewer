"""锚点 ↔ 页码 双向换算（设计 2.3 / 3.3）。

锚点单调递增，因此"锚点 → 页码"用二分查找；越界时收敛到最近页并置
``clamped=true``（客户端据此提示"内容已变化"）。
"""

from __future__ import annotations

from typing import Optional, Tuple

from ..models.schemas import Anchor
from .engine import AnchorTuple, PageMap


def as_tuple(anchor: Anchor | AnchorTuple | Tuple[int, int, int] | None) -> AnchorTuple:
    if anchor is None:
        return (0, 0, 0)
    if isinstance(anchor, Anchor):
        return (anchor.chapter, anchor.block, anchor.char)
    ch, blk, ch_ = anchor
    return (int(ch), int(blk), int(ch_))


def anchor_of_page(pm: PageMap, page: int) -> Anchor:
    rec = pm.record(page)
    if rec is None:
        if pm.pages:
            rec = pm.pages[-1] if page > pm.total_pages else pm.pages[0]
        else:
            return Anchor()
    return Anchor(chapter=rec.anchor[0], block=rec.anchor[1], char=rec.anchor[2])


def page_of_anchor(
    pm: PageMap, anchor: Anchor | AnchorTuple | None
) -> Tuple[int, bool]:
    """返回 (页码, 是否越界收敛)。"""
    target = as_tuple(anchor)
    pages = pm.pages
    if not pages:
        return 1, True
    first = pages[0].anchor
    if target < first:
        return 1, target != first
    if target >= pm.end_anchor:
        # 越过文档末尾（内容已变化 / 进度来自旧版本）→ 收敛到最后一页
        return pages[-1].page, True
    lo, hi = 0, len(pages) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if pages[mid].anchor <= target:
            lo = mid
        else:
            hi = mid - 1
    return pages[lo].page, False


def locate(pm: PageMap, anchor: Anchor | AnchorTuple | None) -> dict:
    """锚点 → 页码 + 页码所属章节。

    **章节以"定位到的那一页"为准**（即 ``record.chapter_index``），这样 ``/locate``
    与 ``/pages/{n}/meta`` 永远给出一致的章节，客户端的章节名与目录高亮不会互相矛盾。

    旧实现扫描"最后一个 start_page ≤ page 的章节"并覆盖 ``record.chapter_index``：
    在 ``chapter_new_page=false``（一章可以从页中间开始）时，一页里可能有多个章节起始，
    于是会报出一个"从这一页才开始、但页首不属于它"的章节。

    锚点自身所属的章节通过 ``anchor_chapter_index`` 单独给出 —— 需要"这个位置属于哪一章"
    的调用方用它，而不必牺牲与 meta 的一致性。
    """
    target = as_tuple(anchor)
    page, clamped = page_of_anchor(pm, target)
    rec = pm.record(page)
    total = max(1, pm.total_pages)

    chapter_index = rec.chapter_index if rec else 0
    chapter_title = ""
    for ch in pm.chapters:
        if int(ch.get("index", -1)) == chapter_index:
            chapter_title = str(ch.get("title", "") or "")
            break
    if not chapter_title and rec:
        chapter_title = rec.chapter_title

    # 锚点自身所属章节：只在文档内且章节表里确实有这一章时才给
    anchor_chapter = chapter_index
    if not clamped:
        for ch in pm.chapters:
            if int(ch.get("index", -1)) == target[0]:
                anchor_chapter = target[0]
                break

    return {
        "page": page,
        "total_pages": pm.total_pages,
        "ratio": min(1.0, max(0.0, (page - 0.5) / total)),
        "chapter_index": chapter_index,
        "chapter_title": chapter_title,
        "anchor_chapter_index": anchor_chapter,
        "clamped": bool(clamped),
    }


def chapter_start_page(pm: PageMap, chapter_index: int) -> Optional[int]:
    for ch in pm.chapters:
        if int(ch.get("index", -1)) == chapter_index:
            start = ch.get("start_page")
            return int(start) if start else None
    return None


def page_ratio(page: int, total_pages: int) -> float:
    total = max(1, total_pages)
    return min(1.0, max(0.0, (page - 0.5) / total))
