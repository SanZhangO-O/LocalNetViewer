"""锚点往返与分页一致性（设计 10.3：锚点往返 / 分页边界）。

这是整个项目最核心的契约：**改字号 / 旋转屏幕后不失位**。
"""

from __future__ import annotations

import pytest

from app.layout.anchors import anchor_of_page, chapter_start_page, locate, page_of_anchor
from app.layout.engine import FlowLayoutEngine
from tests.conftest import make_doc, make_profile


def _build(doc, fonts, **kwargs):
    profile = make_profile(**kwargs)
    engine = FlowLayoutEngine(doc, profile, fonts)
    return profile, engine.build(layout_id="Ltest")


# --------------------------------------------------------------- 自洽性
def test_every_page_anchor_maps_back_to_itself(doc, fonts):
    """页首锚点 → 页码 必须回到原页（同一 layout 内恒等）。"""
    _, pm = _build(doc, fonts)
    assert pm.total_pages > 1, "测试文档至少要分出多页才有意义"
    for rec in pm.pages:
        page, clamped = page_of_anchor(pm, rec.anchor)
        assert page == rec.page, f"第 {rec.page} 页的锚点回到了第 {page} 页"
        assert clamped is False


def test_page_anchors_are_strictly_increasing(doc, fonts):
    """锚点必须严格递增 —— 二分查找与"锚点可比大小"都建立在这一点上。"""
    _, pm = _build(doc, fonts)
    anchors = [rec.anchor for rec in pm.pages]
    for prev, cur in zip(anchors, anchors[1:]):
        assert cur > prev, f"锚点未前进: {prev} -> {cur}"


def test_anchor_of_page_and_page_of_anchor_agree(doc, fonts):
    _, pm = _build(doc, fonts)
    for rec in pm.pages:
        anchor = anchor_of_page(pm, rec.page)
        assert page_of_anchor(pm, anchor)[0] == rec.page


def test_all_anchors_stay_inside_document(doc, fonts):
    _, pm = _build(doc, fonts)
    for rec in pm.pages:
        chapter, block, char = rec.anchor
        assert 0 <= chapter < len(doc.chapters)
        assert 0 <= block <= len(doc.chapters[chapter].blocks)
        assert char >= 0


# --------------------------------------------------------- 重新排版不失位
@pytest.mark.parametrize("size", [32, 40, 52, 72])
def test_anchor_survives_font_size_change(doc, fonts, size):
    """在 A 布局下取到的页首锚点，在 B 布局下必须落在"包含该位置"的那一页。

    判定方式：B 里定位到的页，其页首锚点必须 ≤ 原锚点，且该页的下一页页首锚点
    必须 > 原锚点（即原锚点确实落在这一页的区间内）。
    """
    _, pm_a = _build(doc, fonts, size=40)
    _, pm_b = _build(doc, fonts, size=size)
    if pm_b.total_pages < 2:
        pytest.skip("B 布局页数太少")

    for rec in pm_a.pages:
        page, clamped = page_of_anchor(pm_b, rec.anchor)
        assert not clamped, f"锚点 {rec.anchor} 在 B 布局被判为越界"
        target = pm_b.record(page)
        assert target is not None
        assert target.anchor <= rec.anchor
        nxt = pm_b.record(page + 1)
        if nxt is not None:
            assert rec.anchor < nxt.anchor, (
                f"锚点 {rec.anchor} 落在第 {page} 页区间之外（下一页锚点 {nxt.anchor}）"
            )


def test_anchor_survives_viewport_rotation(doc, fonts):
    """旋转屏幕（宽高互换）后锚点仍能定位。"""
    _, portrait = _build(doc, fonts, w=1080, h=1920)
    _, landscape = _build(doc, fonts, w=1920, h=1080)
    for rec in portrait.pages[:20]:
        page, clamped = page_of_anchor(landscape, rec.anchor)
        assert not clamped
        assert 1 <= page <= landscape.total_pages


def test_line_height_change_keeps_anchor_valid(doc, fonts):
    _, pm_a = _build(doc, fonts, line_height=1.6)
    _, pm_b = _build(doc, fonts, line_height=2.2)
    for rec in pm_a.pages[:20]:
        assert page_of_anchor(pm_b, rec.anchor)[1] is False


# --------------------------------------------------------------- 收敛
def test_locate_clamps_before_start(doc, fonts):
    _, pm = _build(doc, fonts)
    result = locate(pm, (-5, 0, 0))
    assert result["page"] == 1
    assert result["clamped"] is True


def test_locate_clamps_past_end(doc, fonts):
    _, pm = _build(doc, fonts)
    result = locate(pm, (9999, 9999, 9999))
    assert result["page"] == pm.pages[-1].page
    assert result["clamped"] is True


def test_locate_first_anchor_is_not_clamped(doc, fonts):
    _, pm = _build(doc, fonts)
    result = locate(pm, (0, 0, 0))
    assert result["page"] == 1
    assert result["clamped"] is False


def test_locate_empty_pagemap():
    from app.layout.engine import PageMap

    pm = PageMap(book_id="b", layout_id="L", total_pages=0)
    assert page_of_anchor(pm, (0, 0, 0)) == (1, True)
    result = locate(pm, (0, 0, 0))
    assert result["page"] == 1
    assert result["total_pages"] == 0


def test_ratio_is_within_unit_interval(doc, fonts):
    _, pm = _build(doc, fonts)
    for rec in pm.pages:
        ratio = locate(pm, rec.anchor)["ratio"]
        assert 0.0 <= ratio <= 1.0
    assert locate(pm, pm.pages[0].anchor)["ratio"] < locate(pm, pm.pages[-1].anchor)["ratio"]


# --------------------------------------------------------------- 章节
def test_chapter_start_pages_are_monotonic(doc, fonts):
    _, pm = _build(doc, fonts)
    starts = [ch["start_page"] for ch in pm.chapters if ch.get("start_page")]
    assert starts == sorted(starts)
    assert starts[0] == 1


def test_chapter_new_page_forces_break(doc, fonts):
    """chapter_new_page=true 时每一章都必须从新页开始。"""
    _, pm = _build(doc, fonts, chapter_new_page=True)
    for ch in pm.chapters:
        start = ch["start_page"]
        assert start is not None
        rec = pm.record(start)
        assert rec is not None
        assert rec.anchor[1] == 0, "章起始页的页首锚点必须是该章第 0 块"
        assert rec.anchor[2] == 0


def test_chapter_start_page_lookup(doc, fonts):
    _, pm = _build(doc, fonts)
    for ch in pm.chapters:
        assert chapter_start_page(pm, ch["index"]) == ch["start_page"]
    assert chapter_start_page(pm, 999) is None


def test_heading_reachable_through_chapter_list(doc, fonts):
    """点目录跳章后，该章标题必须出现在目标页上。"""
    profile, pm = _build(doc, fonts)
    assert pm.total_pages >= len(doc.chapters)
    for ch in pm.chapters:
        engine = FlowLayoutEngine(doc, profile, fonts)
        laid = engine.layout_page(pm.record(ch["start_page"]).anchor)
        texts = [getattr(i, "text", "") for i in laid.items]
        assert any(ch["title"] in t for t in texts), f"第 {ch['index']} 章标题不在起始页"


# --------------------------------------------------------------- 边界
def test_single_block_longer_than_a_page(fonts):
    """一个块比整页还高时必须能正常分页，不能死循环。"""
    doc = make_doc(chapters=0)
    doc.chapters.append(
        __import__("app.formats.base", fromlist=["Chapter"]).Chapter(
            index=0,
            title="超长",
            blocks=[
                __import__("app.formats.base", fromlist=["Block"]).Block.paragraph("很长的正文。" * 4000)
            ],
        )
    )
    _, pm = _build(doc, fonts)
    assert pm.total_pages > 1


def test_empty_document_yields_zero_pages(fonts):
    from app.formats.base import DocumentModel

    doc = DocumentModel(book_id="empty", format="txt")
    _, pm = _build(doc, fonts)
    assert pm.total_pages == 0
    assert pm.pages == []


def test_blank_paragraph_is_a_one_line_spacer(fonts):
    """纯空白段落（EPUB 里常见的 `<p>&nbsp;</p>` 间隔块）的确是"占一行的间隔"。

    把语义钉住：它占一行高度，但**不会**为每个空段落各起一页；真正没有任何可见
    内容的文档仍然是 0 页。空字符串段落则不占高度。
    """
    from app.formats.base import Block, Chapter, DocumentModel

    doc = DocumentModel(book_id="blank", format="txt")
    doc.chapters.append(
        Chapter(
            index=0,
            title="空章",
            blocks=[Block.paragraph("   "), Block.paragraph(""), Block.paragraph("正文")],
        )
    )
    _, pm = _build(doc, fonts)
    assert pm.total_pages == 1
    # 空白段落缩进后是"两个全角空格"，其后紧跟正文
    assert pm.pages[0].anchor == (0, 0, 0)
    assert pm.total_pages == 1


def test_only_blank_paragraphs_still_terminates(fonts):
    """全是空白段落时也必须有限终止（不能死循环），页数与段落数同量级。"""
    from app.formats.base import Block, Chapter, DocumentModel

    count = 50
    doc = DocumentModel(book_id="blank2", format="txt")
    doc.chapters.append(
        Chapter(index=0, title="空章", blocks=[Block.paragraph(" " * 10) for _ in range(count)])
    )
    _, pm = _build(doc, fonts)
    assert 0 < pm.total_pages <= count
    anchors = [rec.anchor for rec in pm.pages]
    assert anchors == sorted(set(anchors)), "页首锚点必须严格递增"


def test_pagemap_round_trip_through_dict(doc, fonts):
    """落盘 / 读盘后锚点与页数必须完全一致（否则重启后进度会漂）。"""
    from app.layout.engine import PageMap

    _, pm = _build(doc, fonts)
    payload = pm.to_dict()
    restored = PageMap.from_dict(payload)
    assert restored.total_pages == pm.total_pages
    assert [p.anchor for p in restored.pages] == [p.anchor for p in pm.pages]
    assert restored.end_anchor == pm.end_anchor
    for rec in restored.pages:
        assert page_of_anchor(restored, rec.anchor)[0] == rec.page


def test_pagemap_recovers_missing_end_anchor(fonts):
    """旧格式 / 被裁剪的 PageMap 缺少 end_anchor 时不能把整本书判为越界。"""
    from app.layout.engine import PageMap

    pm = PageMap(
        book_id="b",
        layout_id="L",
        total_pages=2,
        pages=[
            __import__("app.layout.engine", fromlist=["PageRecord"]).PageRecord(1, (0, 0, 0)),
            __import__("app.layout.engine", fromlist=["PageRecord"]).PageRecord(2, (0, 5, 0)),
        ],
    )
    data = pm.to_dict()
    data.pop("end_anchor")
    restored = PageMap.from_dict(data)
    assert restored.end_anchor > (0, 5, 0)
    assert page_of_anchor(restored, (0, 5, 0)) == (2, False)


# --------------------------------------------------------- 固定版式锚点
def test_fixed_pagemap_anchor_is_page_index():
    from app.layout.engine import build_fixed_pagemap

    pm = build_fixed_pagemap("b", "L", 7)
    assert pm.total_pages == 7
    for i, rec in enumerate(pm.pages):
        assert rec.anchor == (0, i, 0)
        assert page_of_anchor(pm, rec.anchor) == (i + 1, False)
    assert pm.end_anchor == (0, 7, 0)
    # 越界收敛到最后一页
    assert page_of_anchor(pm, (0, 99, 0)) == (7, True)


def test_strip_pagemap_anchor_encodes_y0():
    from app.formats.base import StripPage
    from app.layout.engine import build_strip_pagemap

    slices = [StripPage(image_index=0, y0=0, height=1000), StripPage(image_index=0, y0=1000, height=1000)]
    pm = build_strip_pagemap("b", "L", slices)
    assert pm.mode == "strip"
    assert pm.pages[0].anchor == (0, 0, 0)
    assert pm.pages[1].anchor == (0, 0, 1000)
    assert pm.end_anchor == (0, 0, 2000)
    assert page_of_anchor(pm, (0, 0, 1000)) == (2, False)
