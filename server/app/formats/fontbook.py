"""字体字样预览适配器（M5 Phase 5）：ttf/otf/ttc 渲染成一页字样样本。

低风险的"文件类型扩展"：字体不是电子书，但能以固定版式单页打开，
方便在手机上快速预览某个字体文件。
"""

from __future__ import annotations

from typing import Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

from ..util import imaging
from ..util.paths import stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    BookRef,
    FixedAdapter,
    ParseError,
)

PAGE_W = 1000
PAGE_H = 1414

_SAMPLE_CJK = "永和九年，岁在癸丑，暮春之初"
_SAMPLE_CJK2 = "汉字字体预览：山川异域，风月同天"
_SAMPLE_ASCII = "The quick brown fox jumps over the lazy dog"
_SAMPLE_NUM = "ABCDEFGHIJKLM abcdefghijklm 0123456789 !@#$%&*()"


class FontSampleAdapter(FixedAdapter):
    format = "font"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "font").lower()
        self._cache: Optional[Tuple[bytes, str]] = None

    def page_count(self) -> int:
        return 1

    def meta(self) -> AdapterMeta:
        return AdapterMeta(
            title=f"{stem_of(self.ref.name)} 字样",
            format=self.format,
            mode="fixed",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=1,
            chapters=[],
            supports_json=False,
        )

    def page_native_size(self, page: int) -> Tuple[int, int]:
        return (PAGE_W, PAGE_H)

    def _load_font(self, size: int):
        try:
            if self.ref.path.suffix.lower() == ".ttc":
                return ImageFont.truetype(str(self.ref.path), size, index=0)
            return ImageFont.truetype(str(self.ref.path), size)
        except Exception as exc:
            raise ParseError(f"字体无法加载: {exc}") from exc

    def _render(self) -> bytes:
        if self._cache is not None:
            return self._cache[0]
        img = Image.new("RGB", (PAGE_W, PAGE_H), (255, 255, 255))
        draw = ImageDraw.Draw(img)
        y = 60
        lines = [
            (f"{stem_of(self.ref.name)}", 64),
            ("", 24),
            (_SAMPLE_CJK, 56),
            (_SAMPLE_CJK2, 44),
            ("", 16),
            (_SAMPLE_ASCII, 34),
            (_SAMPLE_NUM, 30),
        ]
        for text, size in lines:
            if not text:
                y += size
                continue
            font = self._load_font(size)
            try:
                draw.text((60, y), text, font=font, fill=(20, 20, 20))
            except Exception as exc:
                raise ParseError(f"字样渲染失败: {exc}") from exc
            y += int(size * 1.8)
            if y > PAGE_H - 80:
                break
        from io import BytesIO

        buf = BytesIO()
        img.save(buf, format="PNG", optimize=True)
        data = buf.getvalue()
        self._cache = (data, "image/png")
        return data

    def page_bytes(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        data = self._render()
        target_w = max(1, int(round(viewport_w * max(0.5, min(2.0, render_scale)))))
        if target_w < PAGE_W:
            try:
                img = imaging.open_image(data)
                img = imaging.fit_width(img, target_w)
                return imaging.encode_image(img, prefer_jpeg=True, quality=quality or 85)
            except Exception:
                pass
        return data, "image/png"

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        try:
            return self.page_bytes(1, viewport_w=max(80, width or 320))
        except Exception:
            return None
