"""PDF 适配器：pypdfium2 直渲（固定版式，设计 7.4）。

- 不进入文本分页：``layout`` 返回 mode=fixed 与页数；
- ``fit=viewport``：按客户端视口宽度等比渲染后编码 JPEG；
- ``fit=original``：以 2 px/pt（约 144dpi）渲染，保真查看；
- 渲染倍率受 ``render.max_page_pixels`` 限制，越限自动降倍。
"""

from __future__ import annotations

from typing import Dict, Optional, Tuple

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

DEFAULT_RENDER_PT_TO_PX = 2.0  # fit=original 时的基准倍率


class PdfAdapter(FixedAdapter):
    format = "pdf"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        try:
            import pypdfium2 as pdfium  # noqa: PLC0415
        except Exception as exc:  # pragma: no cover - 依赖缺失
            raise UnsupportedFormatError(
                "PDF 支持需要 pypdfium2，请执行 pip install pypdfium2", reason="缺少 pypdfium2"
            ) from exc
        self._pdfium = pdfium
        try:
            self._doc = pdfium.PdfDocument(str(ref.path))
        except Exception as exc:
            raise ParseError(f"PDF 无法打开: {exc}") from exc
        self._size_cache: Dict[int, Tuple[int, int]] = {}
        # 注意：单页像素上限在 render 段（不是 limits 段）
        self.max_pixels = int(env.config.render.max_page_pixels or 0)

    def close(self) -> None:
        try:
            self._doc.close()
        except Exception:
            pass

    # ------------------------------------------------------------ meta
    def _meta_dict(self) -> Dict[str, str]:
        out: Dict[str, str] = {}
        try:
            data = self._doc.get_metadata_dict()
            if isinstance(data, dict):
                out = {str(k): str(v) for k, v in data.items() if v}
        except Exception:
            for key in ("Title", "Author", "Subject", "Keywords"):
                try:
                    val = self._doc.get_metadata_value(key)
                except Exception:
                    val = None
                if val:
                    out[key] = str(val)
        return out

    def page_count(self) -> int:
        try:
            return int(len(self._doc))
        except Exception as exc:
            raise ParseError(f"PDF 页数读取失败: {exc}") from exc

    def meta(self) -> AdapterMeta:
        md = self._meta_dict()
        title = (md.get("Title") or "").strip() or stem_of(self.ref.name)
        author = (md.get("Author") or "").strip() or None
        return AdapterMeta(
            title=title,
            format=self.format,
            mode="fixed",
            author=author,
            language=None,
            size=self.ref.size,
            page_count=self.page_count(),
            chapters=[],
            supports_json=False,
        )

    # ------------------------------------------------------------ 尺寸
    def page_native_size(self, page: int) -> Tuple[int, int]:
        """返回该页在 fit=original 基准倍率下的像素尺寸。"""
        key = int(page)
        if key in self._size_cache:
            return self._size_cache[key]
        n = self.page_count()
        if page < 1 or page > n:
            raise UnsupportedFormatError(f"页码越界: {page}")
        try:
            pdf_page = self._doc[page - 1]
            w_pt, h_pt = pdf_page.get_size()
        except Exception as exc:
            raise ParseError(f"PDF 页面尺寸读取失败: {exc}") from exc
        size = (
            max(1, int(round(w_pt * DEFAULT_RENDER_PT_TO_PX))),
            max(1, int(round(h_pt * DEFAULT_RENDER_PT_TO_PX))),
        )
        self._size_cache[key] = size
        return size

    def page_aspect(self, page: int) -> float:
        try:
            pdf_page = self._doc[page - 1]
            w_pt, h_pt = pdf_page.get_size()
            return (h_pt / w_pt) if w_pt else 0.0
        except Exception:
            return 0.0

    # ------------------------------------------------------------ 取页
    def _render_pil(self, page: int, scale: float):
        try:
            pdf_page = self._doc[page - 1]
            bitmap = pdf_page.render(scale=float(scale))
            img = bitmap.to_pil()
        except Exception as exc:
            raise ParseError(f"PDF 第 {page} 页渲染失败: {exc}") from exc
        try:
            img.load()
        except Exception:
            pass
        return img

    def _clamp_scale(self, w_pt: float, h_pt: float, scale: float) -> float:
        if self.max_pixels <= 0 or w_pt <= 0 or h_pt <= 0:
            return scale
        pixels = (w_pt * scale) * (h_pt * scale)
        if pixels <= self.max_pixels:
            return scale
        return scale * ((self.max_pixels / pixels) ** 0.5)

    def page_bytes(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        n = self.page_count()
        if page < 1 or page > n:
            raise UnsupportedFormatError(f"页码越界: {page}")
        try:
            pdf_page = self._doc[page - 1]
            w_pt, h_pt = pdf_page.get_size()
        except Exception as exc:
            # 损坏的 PDF 可能"能打开"但取页失败；pypdfium2 的异常类型不在错误契约里，
            # 不包一层就会变成 500 INTERNAL
            raise ParseError(f"PDF 第 {page} 页无法读取: {exc}") from exc
        w_pt = float(w_pt) or 1.0
        h_pt = float(h_pt) or 1.0
        if fit == "original":
            scale = DEFAULT_RENDER_PT_TO_PX * max(0.5, min(2.0, render_scale))
        else:
            scale = (max(1, viewport_w) * max(0.5, min(2.0, render_scale))) / w_pt
        scale = self._clamp_scale(w_pt, h_pt, max(0.05, scale))
        img = self._render_pil(page, scale)
        prefer_jpeg = True
        return imaging.encode_image(img, prefer_jpeg=prefer_jpeg, quality=quality)

    # ------------------------------------------------------------ 封面
    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        if self.page_count() < 1:
            return None
        data, mime = self.page_bytes(1, fit="viewport", quality=82, viewport_w=width or 480)
        return data, mime

    # ------------------------------------------------------------ 跨页
    def pair_bytes(
        self,
        page: int,
        *,
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
        rtl: bool = False,
    ) -> Tuple[bytes, str]:
        n = self.page_count()
        first = page
        second = page + 1
        if second > n:
            return self.page_bytes(
                first, fit="viewport", quality=quality, viewport_w=viewport_w, render_scale=render_scale
            )
        imgs = []
        for p in (first, second):
            try:
                pdf_page = self._doc[p - 1]
                w_pt, h_pt = pdf_page.get_size()
            except Exception as exc:
                raise ParseError(f"PDF 第 {p} 页无法读取: {exc}") from exc
            w_pt = float(w_pt) or 1.0
            scale = self._clamp_scale(w_pt, float(h_pt) or 1.0, (viewport_w * render_scale) / w_pt)
            imgs.append(self._render_pil(p, max(0.05, scale)))
        if rtl:
            imgs = list(reversed(imgs))
        merged = imaging.hstack(imgs)
        return imaging.encode_image(merged, prefer_jpeg=True, quality=quality)
