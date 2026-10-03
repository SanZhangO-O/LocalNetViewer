"""Pillow 栅格化渲染（设计 4.7）。

- 画布 = ``viewport × render_scale``，受 ``render.max_page_pixels`` 限制；
- 绘制顺序：主题背景 → 文本行 → 图片（等比缩放、居中）；
- 页码 / 章节名不由服务器绘制（客户端叠加底栏，服务器按 ``margin.bottom`` 预留）；
- 文本为主页输出 PNG，照片为主页输出 JPEG q85（可用配置关闭）；
- 行位置/字号与分页引擎完全同源（同一个 ``layout_page``），保证"所见即分页"。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont

from ..formats.base import BookAdapter, DocumentModel
from ..layout.engine import (
    FlowLayoutEngine,
    ImageItem,
    Item,
    PageMap,
    PageRecord,
    SeparatorItem,
    TextItem,
)
from ..layout.fonts import FontManager
from ..models.schemas import LayoutProfile
from ..util import imaging
from ..util.textutil import stable_hash

AssetLoader = Callable[[str], Optional[bytes]]

THEMES: Dict[str, Tuple[str, str]] = {
    "white": ("#FFFFFF", "#222222"),
    "paper": ("#F5F0E6", "#2B2B2B"),
    "sepia": ("#F0E2C8", "#3B2E1E"),
    "green": ("#CCE8CF", "#1F2A1F"),
    "dark": ("#111418", "#C8C8C8"),
}

COVER_PALETTE = (
    ("#2F4858", "#FFFFFF"),
    ("#33658A", "#F6F7F8"),
    ("#5B5F97", "#FFFFFF"),
    ("#7A4E48", "#FFF6EE"),
    ("#386641", "#F2F7F4"),
    ("#6B4226", "#FFF3E4"),
)


@dataclass(frozen=True)
class Theme:
    name: str
    bg: Tuple[int, int, int]
    fg: Tuple[int, int, int]

    @property
    def muted(self) -> Tuple[int, int, int]:
        return _blend(self.fg, self.bg, 0.45)


def hex_to_rgb(value: str) -> Tuple[int, int, int]:
    v = value.lstrip("#")
    return (int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16))


def _blend(a: Tuple[int, int, int], b: Tuple[int, int, int], ratio: float) -> Tuple[int, int, int]:
    r = max(0.0, min(1.0, ratio))
    return (
        int(round(a[0] * (1 - r) + b[0] * r)),
        int(round(a[1] * (1 - r) + b[1] * r)),
        int(round(a[2] * (1 - r) + b[2] * r)),
    )


def theme_of(name: Optional[str]) -> Theme:
    key = (name or "white").strip().lower()
    bg, fg = THEMES.get(key, THEMES["white"])
    return Theme(name=key if key in THEMES else "white", bg=hex_to_rgb(bg), fg=hex_to_rgb(fg))


def clamp_scale(scale: float, profile: LayoutProfile, max_page_pixels: int) -> float:
    s = max(0.5, min(2.0, float(scale or 1.0)))
    if not max_page_pixels:
        return s
    while s > 0.5:
        w = profile.viewport.w * s
        h = profile.viewport.h * s
        if w * h <= max_page_pixels:
            break
        s = round(s - 0.05, 2)
    return max(0.5, s)


def is_photo_page(items: Sequence[Item], content_area: float, avail_w: float = 0.0) -> bool:
    """照片页判定（决定 PNG 还是 JPEG，设计 4.7）。

    单纯按"图片面积 ≥ 内容区一半"会漏判：竖屏视口下一页 4:3 的照片
    按宽度缩放后只占约 1/3 面积。因此再加一条"没有任何文本行 + 图片几乎占满宽度"。
    """
    if not items or content_area <= 0:
        return False
    area = 0.0
    has_text = False
    max_w = 0.0
    for item in items:
        if isinstance(item, ImageItem):
            area += float(item.display_w) * float(item.display_h)
            max_w = max(max_w, float(item.display_w))
        elif isinstance(item, TextItem):
            has_text = True
    if area >= content_area * 0.5:
        return True
    return (not has_text) and avail_w > 0 and max_w >= avail_w * 0.8


# ------------------------------------------------------------------ 页面渲染
def render_flow_page(
    doc: DocumentModel,
    pagemap: PageMap,
    page: int,
    profile: LayoutProfile,
    fonts: FontManager,
    asset_loader: Optional[AssetLoader] = None,
    *,
    theme_name: str = "white",
    scale: float = 1.0,
    max_page_pixels: int = 4_000_000,
    max_source_pixels: int = 30_000_000,
    jpeg_for_photo_pages: bool = True,
) -> Tuple[bytes, str]:
    """渲染 flow 模式的一页，返回 (图片字节, mime)。"""
    record = pagemap.record(page)
    if record is None:
        raise IndexError(f"页码越界: {page}")
    theme = theme_of(theme_name)
    engine = FlowLayoutEngine(doc, profile, fonts)
    laid = engine.layout_page(record.anchor)

    s = clamp_scale(scale, profile, max_page_pixels)
    width = max(1, int(round(profile.viewport.w * s)))
    height = max(1, int(round(profile.viewport.h * s)))
    canvas = Image.new("RGB", (width, height), theme.bg)
    draw = ImageDraw.Draw(canvas)

    left = float(profile.margin.left) * s
    top = float(profile.margin.top) * s
    avail_w = float(profile.avail_width) * s
    spacing = float(profile.font.letter_spacing_px) * s
    family = profile.font.family

    for item in laid.items:
        y = top + float(item.y) * s
        if isinstance(item, TextItem):
            _draw_text_item(draw, item, fonts, family, left, y, s, spacing, theme)
        elif isinstance(item, ImageItem):
            _draw_image_item(
                canvas, item, asset_loader, left, y, avail_w, s, max_source_pixels
            )
        elif isinstance(item, SeparatorItem):
            line_y = y + float(item.height) * s / 2.0
            draw.line(
                [
                    (left + avail_w * 0.08, line_y),
                    (left + avail_w * 0.92, line_y),
                ],
                fill=theme.muted,
                width=max(1, int(round(s))),
            )

    content_area = float(profile.avail_width) * float(profile.avail_height)
    prefer_jpeg = jpeg_for_photo_pages and is_photo_page(
        laid.items, content_area, float(profile.avail_width)
    )
    return imaging.encode_image(canvas, prefer_jpeg=prefer_jpeg, quality=85)


def _draw_text_item(
    draw: ImageDraw.ImageDraw,
    item: TextItem,
    fonts: FontManager,
    family: Optional[str],
    left: float,
    y: float,
    scale: float,
    spacing: float,
    theme: Theme,
) -> None:
    font_size = max(4, int(round(item.font_size * scale)))
    font = fonts.font(family, font_size)
    try:
        ascent, descent = font.getmetrics()
        glyph_h = ascent + descent
    except Exception:
        glyph_h = font_size
    box_h = float(item.height) * scale
    ty = y + max(0.0, (box_h - glyph_h) / 2.0)
    x = left
    text = item.text
    if not text:
        return
    if abs(spacing) > 0.01:
        cx = x
        for ch in text:
            draw.text((cx, ty), ch, font=font, fill=theme.fg)
            try:
                cx += float(font.getlength(ch)) + spacing
            except Exception:
                cx += font_size * (1.0 if ch.strip() else 0.5) + spacing
    else:
        draw.text((x, ty), text, font=font, fill=theme.fg)
    if item.bold:
        # 模拟加粗：轻微偏移二次绘制（不改变行宽，与分页一致）
        offset = max(1.0, font_size / 32.0)
        if abs(spacing) > 0.01:
            cx = x + offset
            for ch in text:
                draw.text((cx, ty), ch, font=font, fill=theme.fg)
                try:
                    cx += float(font.getlength(ch)) + spacing
                except Exception:
                    cx += font_size + spacing
        else:
            draw.text((x + offset, ty), text, font=font, fill=theme.fg)


def _draw_image_item(
    canvas: Image.Image,
    item: ImageItem,
    asset_loader: Optional[AssetLoader],
    left: float,
    y: float,
    avail_w: float,
    scale: float,
    max_source_pixels: int = 30_000_000,
) -> None:
    target_w = max(1, int(round(item.display_w * scale)))
    target_h = max(1, int(round(item.display_h * scale)))
    x = left + max(0.0, (avail_w - target_w) / 2.0)
    data = None
    if asset_loader is not None and item.asset:
        try:
            data = asset_loader(item.asset)
        except Exception:
            data = None
    if not data:
        _draw_missing_image(canvas, x, y, target_w, target_h)
        return
    try:
        pil = imaging.decode_for_display(
            data, target_w, max_source_pixels, prefer_jpeg_draft=True
        )
        pil = pil.resize((target_w, target_h), Image.LANCZOS)
    except Exception:
        _draw_missing_image(canvas, x, y, target_w, target_h)
        return
    if pil.mode in ("RGBA", "LA") or (pil.mode == "P" and "transparency" in pil.info):
        rgba = pil.convert("RGBA")
        canvas.paste(rgba.convert("RGB"), (int(round(x)), int(round(y))), rgba.getchannel("A"))
    else:
        canvas.paste(pil.convert("RGB"), (int(round(x)), int(round(y))))


def _draw_missing_image(canvas: Image.Image, x: float, y: float, w: int, h: int) -> None:
    draw = ImageDraw.Draw(canvas)
    box = [int(round(x)), int(round(y)), int(round(x + w)), int(round(y + h))]
    draw.rectangle(box, outline=(150, 150, 150), width=1)
    label = "图片无法显示"
    try:
        font = default_font(max(10, min(28, h // 6 or 12)))
    except Exception:
        font = None
    try:
        draw.text((int(round(x + 8)), int(round(y + 8))), label, fill=(120, 120, 120), font=font)
    except Exception:
        pass


def default_font(size: int) -> ImageFont.ImageFont:
    """Pillow 内置字体（无中文字形，仅用于占位/兜底）。"""
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # pragma: no cover - 旧版 Pillow
        return ImageFont.load_default()


# ------------------------------------------------------------------ 固定版式
def fixed_page_bytes(
    adapter: BookAdapter,
    record: PageRecord,
    profile: LayoutProfile,
    *,
    fit: str = "viewport",
    quality: int = 85,
    scale: float = 1.0,
    rtl: bool = False,
    pair: bool = False,
    max_page_pixels: int = 0,
) -> Tuple[bytes, str]:
    """PDF / 漫画取页（viewport 缩放或原图直通）。

    ``scale`` 已经折算进 ``viewport_w``，因此这里必须传 ``render_scale=1.0``：
    适配器内部还会再乘一次 render_scale，否则会变成 scale²（例如 2.0 请求渲染成 4 倍）。
    """
    page = record.page
    viewport_w = max(
        1, int(round(profile.viewport.w * clamp_scale(scale, profile, max_page_pixels)))
    )
    if pair:
        return adapter.pair_bytes(
            page, quality=quality, viewport_w=viewport_w, render_scale=1.0, rtl=rtl
        )
    return adapter.page_bytes(
        page, fit=fit, quality=quality, viewport_w=viewport_w, render_scale=1.0
    )


def strip_page_bytes(
    adapter: BookAdapter,
    record: PageRecord,
    profile: LayoutProfile,
    *,
    fit: str = "viewport",
    quality: int = 85,
    scale: float = 1.0,
    max_page_pixels: int = 0,
) -> Tuple[bytes, str]:
    """条漫切片取页（page 即切片序号，图片高度≈视口高度）。"""
    from ..formats.base import StripPage

    sp = StripPage(
        image_index=record.image_index,
        y0=record.y0,
        height=record.height or profile.avail_height,
    )
    viewport_w = max(
        1, int(round(profile.viewport.w * clamp_scale(scale, profile, max_page_pixels)))
    )
    return adapter.strip_slice_bytes(
        sp, quality=quality, viewport_w=viewport_w, render_scale=1.0, fit=fit
    )


# ------------------------------------------------------------------ 封面
def text_cover(
    title: str,
    author: Optional[str] = None,
    *,
    width: int = 320,
    fonts: Optional[FontManager] = None,
    family: Optional[str] = None,
) -> Tuple[bytes, str]:
    """为无封面的书籍（TXT 等）生成标题文字封面（设计 4.8）。"""
    w = max(80, min(1024, int(width or 320)))
    h = int(round(w * 1.42))
    # 必须用稳定哈希：内置 hash() 每进程加盐 → 同一本书重启后封面配色会变
    palette_index = int(stable_hash(title or "", 8), 16) % len(COVER_PALETTE)
    bg_hex, fg_hex = COVER_PALETTE[palette_index]
    bg = hex_to_rgb(bg_hex)
    fg = hex_to_rgb(fg_hex)
    canvas = Image.new("RGB", (w, h), bg)
    draw = ImageDraw.Draw(canvas)

    # 顶部装饰条
    draw.rectangle([0, 0, w, max(2, h // 60)], fill=_blend(fg, bg, 0.75))

    title_size = max(12, int(w * 0.13))
    author_size = max(10, int(w * 0.082))
    title_font = _cover_font(fonts, family, title_size)
    author_font = _cover_font(fonts, family, author_size)

    lines = _wrap_for_cover(title or "无标题", title_font, fonts, family, title_size, w - int(w * 0.16), 4)
    line_h = int(title_size * 1.35)
    total_h = line_h * len(lines)
    y = max(int(h * 0.18), int((h - total_h) / 2) - int(h * 0.06))
    for line in lines:
        draw.text((int(w * 0.08), y), line, font=title_font, fill=fg)
        y += line_h

    if author:
        author_lines = _wrap_for_cover(
            author, author_font, fonts, family, author_size, w - int(w * 0.16), 2
        )
        y = h - int(h * 0.14) - line_h // 2 * len(author_lines)
        for line in author_lines:
            draw.text((int(w * 0.08), y), line, font=author_font, fill=_blend(fg, bg, 0.25))
            y += int(author_size * 1.4)

    return imaging.encode_image(canvas, prefer_jpeg=True, quality=88)


def _cover_font(
    fonts: Optional[FontManager], family: Optional[str], size: int
) -> ImageFont.ImageFont:
    if fonts is not None:
        try:
            return fonts.font(family, size)
        except Exception:
            pass
    return default_font(size)


def _wrap_for_cover(
    text: str,
    font: ImageFont.ImageFont,
    fonts: Optional[FontManager],
    family: Optional[str],
    size: int,
    max_width: float,
    max_lines: int,
) -> List[str]:
    text = " ".join(str(text).split())
    if not text:
        return [""]
    lines: List[str] = []
    cur = ""
    for ch in text:
        probe = cur + ch
        try:
            width = (
                fonts.text_width(probe, family, size)
                if fonts is not None
                else float(font.getlength(probe))  # type: ignore[attr-defined]
            )
        except Exception:
            width = len(probe) * size * 0.6
        if cur and width > max_width:
            lines.append(cur)
            cur = ch
            if len(lines) >= max_lines - 1:
                break
        else:
            cur = probe
    if cur and len(lines) < max_lines:
        lines.append(cur)
    if len(lines) >= max_lines and len("".join(lines)) < len(text):
        lines[-1] = lines[-1][:-1] + "…"
    return lines or [text[:1]]
