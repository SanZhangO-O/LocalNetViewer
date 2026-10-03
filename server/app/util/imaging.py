"""Pillow 图像处理助手：解码防护、EXIF 校正、缩放、编码（设计 4.7）。

要点：
- 按 ``limits.max_source_pixels`` 设置 ``Image.MAX_IMAGE_PIXELS``，防图片炸弹；
- JPEG 大图先 ``Image.draft()`` 降采样再缩放，降低内存峰值；
- 进入渲染前统一 ``ImageOps.exif_transpose``，避免手机拍摄图片横竖颠倒；
- 缩放统一 LANCZOS。
"""

from __future__ import annotations

import io
from typing import Optional, Sequence, Tuple

from PIL import Image, ImageFile, ImageOps, UnidentifiedImageError

# 允许截断图片尽量解码（扫描件常见）
ImageFile.LOAD_TRUNCATED_IMAGES = True

try:  # Pillow >= 9.1
    from PIL import Image as _Image

    _Image.MAX_IMAGE_PIXELS = 30_000_000
except Exception:  # pragma: no cover
    pass


_PNG_EXTS = {".png", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".avif"}

# EXIF Orientation 标签号（0x0112）
EXIF_ORIENTATION_TAG = 0x0112


class ImageTooLargeError(RuntimeError):
    """源图像素超限且无法安全降采样。"""


class ImageDecodeError(RuntimeError):
    """图片无法解码。"""


def set_pixel_limit(max_pixels: int) -> None:
    """设置 Pillow 的解码像素上限（<=0 时保持全局设置不变，避免关闭防护）。"""
    if max_pixels is None or int(max_pixels) <= 0:
        return
    try:
        Image.MAX_IMAGE_PIXELS = int(max_pixels)
    except Exception:  # pragma: no cover
        pass


def set_global_pixel_limit(max_pixels: int) -> None:
    set_pixel_limit(max_pixels)


def mime_for_ext(ext: str) -> str:
    e = (ext or "").lower()
    return {
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".jfif": "image/jpeg",
        ".png": "image/png",
        ".gif": "image/gif",
        ".webp": "image/webp",
        ".bmp": "image/bmp",
        ".avif": "image/avif",
        ".tif": "image/tiff",
        ".tiff": "image/tiff",
    }.get(e, "application/octet-stream")


def encode_ext_for_mime(mime: str) -> str:
    return {
        "image/jpeg": ".jpg",
        "image/png": ".png",
        "image/gif": ".gif",
        "image/webp": ".webp",
        "image/bmp": ".bmp",
        "image/avif": ".avif",
        "image/tiff": ".tiff",
    }.get(mime, ".bin")


def sniff_mime(data: bytes, fallback_ext: str = "") -> str:
    """按魔数判断图片类型，失败则退回扩展名。"""
    if len(data) >= 12:
        if data[:3] == b"\xff\xd8\xff":
            return "image/jpeg"
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            return "image/png"
        if data[:6] in (b"GIF87a", b"GIF89a"):
            return "image/gif"
        if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
            return "image/webp"
        if data[:2] == b"BM":
            return "image/bmp"
        if data[:4] in (b"II*\x00", b"MM\x00*"):
            return "image/tiff"
    return mime_for_ext(fallback_ext)


def open_image(data: bytes) -> Image.Image:
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
        return img
    except (UnidentifiedImageError, OSError, ValueError) as exc:
        raise ImageDecodeError(str(exc)) from exc


def probe_size(data: bytes) -> Optional[Tuple[int, int]]:
    """只读文件头取得 (宽, 高)，不解码像素。

    必须与 :func:`decode_for_display` 的几何一致：后者会做 EXIF 方向校正
    （``ImageOps.exif_transpose``），旋转 90° 的图片宽高会互换。这里同样按
    Orientation 标签交换，否则"分页用的宽高"与"实际画出来的宽高"不一致，
    会出现图片被拉伸、条漫切片错位等问题。
    """
    try:
        with Image.open(io.BytesIO(data)) as img:
            width, height = int(img.width), int(img.height)
            try:
                orientation = img.getexif().get(EXIF_ORIENTATION_TAG)
            except Exception:
                orientation = None
            # 5/6/7/8 = 旋转 90°/270° 或镜像后旋转，长宽互换
            if orientation in (5, 6, 7, 8):
                width, height = height, width
            return width, height
    except Exception:
        return None


def _draft_to(img: Image.Image, target_w: int, target_h: int) -> None:
    """对 JPEG 使用 draft() 在解码阶段降采样（Pillow 会选最接近的 1/2^n）。"""
    try:
        img.draft("RGB", (max(1, target_w), max(1, target_h)))
    except Exception:
        pass


def decode_for_display(
    data: bytes,
    target_w: int,
    max_source_pixels: int = 30_000_000,
    prefer_jpeg_draft: bool = True,
) -> Image.Image:
    """解码图片并校正方向；若超出目标尺寸则按需缩放。

    - ``target_w`` 为期望输出宽度（像素）；
    - JPEG 优先 draft 降采样；
    - 超过 ``max_source_pixels`` 时先 draft，仍超限则抛错（调用方返回 PARSE_FAILED）。
    """
    set_pixel_limit(max_source_pixels)
    try:
        img = Image.open(io.BytesIO(data))
        fmt = (img.format or "").upper()
        src_w, src_h = img.size
        # 1/2 高图先 draft（仅 JPEG；draft 会把尺寸降到 >= 目标）
        if prefer_jpeg_draft and fmt == "JPEG" and target_w > 0:
            target_h = max(1, int(src_h * (target_w / max(1, src_w))))
            _draft_to(img, target_w, target_h)
        img.load()
    except Image.DecompressionBombError as exc:
        raise ImageTooLargeError(str(exc)) from exc
    except (UnidentifiedImageError, OSError, ValueError) as exc:
        raise ImageDecodeError(str(exc)) from exc

    try:
        img = ImageOps.exif_transpose(img)
    except Exception:
        pass
    return img


def fit_width(img: Image.Image, width: int) -> Image.Image:
    """等比缩放到指定宽度（不放大时保持原尺寸，由调用方决定）。"""
    if width <= 0 or img.width == width:
        return img
    height = max(1, round(img.height * (width / img.width)))
    return img.resize((width, height), Image.LANCZOS)


def fit_box(img: Image.Image, box_w: int, box_h: int, allow_upscale: bool = False) -> Image.Image:
    """等比缩放使图片完整放入 box。"""
    if box_w <= 0 or box_h <= 0:
        return img
    ratio = min(box_w / img.width, box_h / img.height)
    if ratio > 1.0 and not allow_upscale:
        ratio = 1.0
    w = max(1, round(img.width * ratio))
    h = max(1, round(img.height * ratio))
    if (w, h) == img.size:
        return img
    return img.resize((w, h), Image.LANCZOS)


def crop_slice(img: Image.Image, y0: int, height: int) -> Image.Image:
    """纵向切片（条漫 strip 模式）：直接裁剪，不缩放。"""
    top = max(0, min(int(y0), max(0, img.height - 1)))
    bottom = min(img.height, top + max(1, int(height)))
    return img.crop((0, top, img.width, bottom))


def to_rgb(img: Image.Image, background: Tuple[int, int, int] = (255, 255, 255)) -> Image.Image:
    if img.mode in ("RGB", "L"):
        return img.convert("RGB") if img.mode == "L" else img
    if img.mode == "P":
        img = img.convert("RGBA")
    if img.mode in ("RGBA", "LA"):
        base = Image.new("RGB", img.size, background)
        alpha = img.getchannel("A")
        base.paste(img.convert("RGB"), (0, 0), alpha)
        return base
    return img.convert("RGB")


def encode_image(
    img: Image.Image,
    prefer_jpeg: bool = True,
    quality: int = 85,
    force_png: bool = False,
) -> Tuple[bytes, str]:
    """编码为 PNG / JPEG，返回 (字节, mime)。带透明度或强制时用 PNG。"""
    has_alpha = img.mode in ("RGBA", "LA", "P") and (
        img.mode != "P" or "transparency" in img.info
    )
    use_png = force_png or (has_alpha and not prefer_jpeg)
    buf = io.BytesIO()
    if use_png:
        out = img if img.mode in ("RGBA", "LA", "P", "L") else img.convert("RGB")
        out.save(buf, format="PNG", optimize=True)
        return buf.getvalue(), "image/png"
    if not prefer_jpeg:
        out = img if img.mode in ("RGB", "L") else to_rgb(img)
        out.save(buf, format="PNG", optimize=True)
        return buf.getvalue(), "image/png"
    rgb = to_rgb(img)
    rgb.save(buf, format="JPEG", quality=int(quality), optimize=True, progressive=True)
    return buf.getvalue(), "image/jpeg"


def encode_png(img: Image.Image) -> Tuple[bytes, str]:
    return encode_image(img, prefer_jpeg=False, force_png=True)


def hstack(images: Sequence[Image.Image], background: Tuple[int, int, int] = (255, 255, 255)) -> Image.Image:
    """横向拼接（双页跨页）；高度对齐到最大值，垂直居中。"""
    imgs = [i for i in images if i is not None]
    if not imgs:
        raise ImageDecodeError("没有可拼接的页面")
    if len(imgs) == 1:
        return imgs[0]
    total_w = sum(i.width for i in imgs)
    max_h = max(i.height for i in imgs)
    canvas = Image.new("RGB", (max(1, total_w), max(1, max_h)), background)
    x = 0
    for img in imgs:
        rgb = to_rgb(img, background)
        canvas.paste(rgb, (x, (max_h - rgb.height) // 2))
        x += img.width
    return canvas


def vstack(images: Sequence[Image.Image], background: Tuple[int, int, int] = (255, 255, 255)) -> Image.Image:
    """纵向拼接（条漫整幅导出用）。"""
    imgs = [i for i in images if i is not None]
    if not imgs:
        raise ImageDecodeError("没有可拼接的页面")
    if len(imgs) == 1:
        return imgs[0]
    max_w = max(i.width for i in imgs)
    total_h = sum(i.height for i in imgs)
    canvas = Image.new("RGB", (max(1, max_w), max(1, total_h)), background)
    y = 0
    for img in imgs:
        rgb = to_rgb(img, background)
        canvas.paste(rgb, ((max_w - rgb.width) // 2, y))
        y += img.height
    return canvas
