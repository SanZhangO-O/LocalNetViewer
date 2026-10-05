"""JSON 模式页面序列化（设计 3.2 / 5.3，P4 扩展）。

关键约定：分页边界永远由服务器决定。服务器按"行"下发（``lines``），客户端逐行
绘制、不做重排，因此 JSON 模式与 PNG 模式页码完全一致；图片块必须按服务器计算的
``display_w/display_h`` 布局，不要按原始尺寸自行换算。
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional
from urllib.parse import quote

from ..formats.base import DocumentModel
from ..layout.engine import FlowLayoutEngine, ImageItem, PageMap, SeparatorItem, TextItem
from ..layout.fonts import FontManager
from ..models.schemas import LayoutProfile


def asset_url(book_id: str, asset: str, revision: str = "") -> str:
    url = f"/api/v1/books/{book_id}/asset?path={quote(asset, safe='')}"
    if revision:
        # 带修订号：内容变化（rev 变）后 URL 变化，客户端强缓存自然失效
        url += f"&rev={quote(str(revision), safe='')}"
    return url


def build_page_json(
    doc: DocumentModel,
    pagemap: PageMap,
    page: int,
    profile: LayoutProfile,
    fonts: FontManager,
    book_id: Optional[str] = None,
    include_font: bool = True,
    revision: str = "",
) -> Dict[str, Any]:
    record = pagemap.record(page)
    if record is None:
        raise IndexError(f"页码越界: {page}")
    bid = book_id or doc.book_id
    engine = FlowLayoutEngine(doc, profile, fonts)
    laid = engine.layout_page(record.anchor)

    blocks: List[Dict[str, Any]] = []
    current_key: Optional[tuple] = None

    def _flush() -> None:
        nonlocal current_key
        current_key = None

    for item in laid.items:
        if isinstance(item, TextItem):
            key = (item.kind, item.chapter, item.block, item.level, item.font_size)
            if key != current_key:
                if item.kind == "heading":
                    block: Dict[str, Any] = {
                        "type": "heading",
                        "level": item.level or 1,
                        "lines": [],
                        "font_size": item.font_size,
                        "y": round(float(item.y), 2),
                    }
                else:
                    block = {
                        "type": "paragraph",
                        "lines": [],
                        "font_size": item.font_size,
                        "y": round(float(item.y), 2),
                    }
                if item.bold:
                    block["bold"] = True
                if item.italic:
                    block["italic"] = True
                blocks.append(block)
                current_key = key
            blocks[-1]["lines"].append(item.text)
            blocks[-1]["height"] = round(
                float(blocks[-1].get("height", 0.0)) + float(item.height), 2
            )
            continue

        _flush()
        if isinstance(item, ImageItem):
            blocks.append(
                {
                    "type": "image",
                    "asset": asset_url(bid, item.asset, revision),
                    "display_w": round(float(item.display_w), 2),
                    "display_h": round(float(item.display_h), 2),
                    "y": round(float(item.y), 2),
                    "height": round(float(item.height), 2),
                }
            )
        elif isinstance(item, SeparatorItem):
            blocks.append(
                {
                    "type": "separator",
                    "y": round(float(item.y), 2),
                    "height": round(float(item.height), 2),
                }
            )

    payload: Dict[str, Any] = {
        "page": record.page,
        "total_pages": pagemap.total_pages,
        "anchor": {"chapter": record.anchor[0], "block": record.anchor[1], "char": record.anchor[2]},
        "chapter_index": record.chapter_index,
        "chapter_title": record.chapter_title,
        "mode": pagemap.mode,
        "blocks": blocks,
    }
    if include_font:
        payload["font"] = {
            "family": profile.font.family,
            "size_px": profile.font.size_px,
            "line_height": profile.font.line_height,
            "letter_spacing_px": profile.font.letter_spacing_px,
        }
        payload["margin"] = {
            "left": profile.margin.left,
            "right": profile.margin.right,
            "top": profile.margin.top,
            "bottom": profile.margin.bottom,
        }
        payload["viewport"] = {"w": profile.viewport.w, "h": profile.viewport.h}
        # 设计 3.2 要求客户端"逐行绘制、不做重排"，因此必须把段落间距等
        # 影响纵向位置的参数一并下发，否则 JSON 模式与 PNG 模式会错位。
        #
        # 但 first_line_indent_chars 必须下发 **0**：上面的 `lines` 里已经带了
        # "\u3000"×indent 的段首缩进（与 PNG 模式同源）。如果这里再报 2，
        # 客户端照着补缩进就会把首行缩进成 4 个全角空格，与 PNG 模式不一致。
        payload["paragraph"] = {
            "spacing_px": profile.paragraph.spacing_px,
            "first_line_indent_chars": 0,
            "indent_applied_in_lines": True,
        }
    return payload
