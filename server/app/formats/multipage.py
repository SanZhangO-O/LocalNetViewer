"""多帧图片适配器（M5 Phase 1）：多页 TIFF 等按"一帧一页"的固定版式打开。

单页 TIFF 同样适用（n_frames=1），所以 TIFF 扩展名不再走"单图图集"路径。
"""

from __future__ import annotations

from typing import Optional, Tuple

from PIL import Image, ImageOps

from ..util import imaging
from ..util.paths import stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    BookRef,
    FixedAdapter,
    ParseError,
    UnsupportedFormatError,
)


class MultiPageImageAdapter(FixedAdapter):
    format = "tiff"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "tiff").lower()
        try:
            self._img = Image.open(ref.path)
            self._frames = int(getattr(self._img, "n_frames", 1) or 1)
        except Exception as exc:
            raise ParseError(f"图片无法打开: {exc}") from exc
        self.jpeg_quality = int(env.config.comic.jpeg_quality)
        self.max_source_pixels = int(env.config.limits.max_source_pixels or 0)

    def close(self) -> None:
        try:
            self._img.close()
        except Exception:
            pass

    # ------------------------------------------------------------ 基础
    def page_count(self) -> int:
        return max(1, int(self._frames))

    def meta(self) -> AdapterMeta:
        return AdapterMeta(
            title=stem_of(self.ref.name),
            format=self.format,
            mode="fixed",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=self.page_count(),
            chapters=[],
            supports_json=False,
        )

    def _frame(self, page: int) -> Image.Image:
        n = self.page_count()
        if page < 1 or page > n:
            raise UnsupportedFormatError(f"页码越界: {page}")
        try:
            self._img.seek(page - 1)
            width, height = int(self._img.width), int(self._img.height)
            if self.max_source_pixels and width * height > self.max_source_pixels:
                raise ParseError(
                    f"第 {page} 帧像素超限（{width}×{height} > {self.max_source_pixels}）"
                )
            frame = self._img.copy()
        except ParseError:
            raise
        except Exception as exc:
            raise ParseError(f"第 {page} 帧读取失败: {exc}") from exc
        try:
            frame = ImageOps.exif_transpose(frame)
        except Exception:
            pass
        return frame

    def page_native_size(self, page: int) -> Tuple[int, int]:
        n = self.page_count()
        if page < 1 or page > n:
            raise UnsupportedFormatError(f"页码越界: {page}")
        try:
            self._img.seek(page - 1)
            return int(self._img.width), int(self._img.height)
        except Exception as exc:
            raise ParseError(f"第 {page} 帧尺寸读取失败: {exc}") from exc

    # ------------------------------------------------------------ 取页
    def page_bytes(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        frame = self._frame(page)
        target_w = max(1, int(round(viewport_w * max(0.5, min(2.0, render_scale)))))
        if fit == "original":
            target_w = max(target_w, frame.width)
        if frame.width > target_w:
            try:
                frame = imaging.fit_width(frame, target_w)
            except Exception as exc:
                raise ParseError(f"图片缩放失败: {exc}") from exc
        prefer_jpeg = not _has_alpha(frame)
        q = quality or self.jpeg_quality
        return imaging.encode_image(frame, prefer_jpeg=prefer_jpeg, quality=q)

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        if self.page_count() < 1:
            return None
        try:
            return self.page_bytes(1, fit="viewport", quality=80, viewport_w=max(80, width or 320))
        except Exception:
            return None


def _has_alpha(img: Image.Image) -> bool:
    try:
        return img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info)
    except Exception:
        return False
