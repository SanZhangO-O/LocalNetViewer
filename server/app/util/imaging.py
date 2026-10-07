"""Pillow 图像处理助手：解码防护、EXIF 校正、缩放、编码（设计 4.7）。

要点：
- 按 ``limits.max_source_pixels`` 设置 ``Image.MAX_IMAGE_PIXELS``，防图片炸弹；
- JPEG 大图先 ``Image.draft()`` 降采样再缩放，降低内存峰值；
- 进入渲染前统一 ``ImageOps.exif_transpose``，避免手机拍摄图片横竖颠倒；
- 缩放统一 LANCZOS。
"""

from __future__ import annotations

import io
import threading
from typing import Mapping, Optional, Sequence, Tuple

from PIL import Image, ImageFile, ImageOps, UnidentifiedImageError

from ..formats.specs import BASE_IMAGE_EXTS  # 图片基础扩展名的单一事实源

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


# ------------------------------------------------------------------ 可选图像后端（M5）
# 单图基础扩展名与异形扩展名；基础清单以 formats/specs.py 为单一事实源
# （AGENTS.md：新增图片扩展名只改 specs）。
_EXOTIC_IMAGE_BACKENDS: Mapping[str, str] = {
    ".heic": "heif",
    ".heif": "heif",
    ".jxl": "jxl",
    ".svg": "svg",
    ".psd": "psd",
    ".cr2": "raw",
    ".cr3": "raw",
    ".nef": "raw",
    ".arw": "raw",
    ".dng": "raw",
    ".orf": "raw",
    ".raf": "raw",
    ".rw2": "raw",
}

_BACKEND_LOCK = threading.Lock()
_BACKEND_READY: dict = {}
_PLUGINS_REGISTERED = False
_SUPPORTED_CACHE: Optional[frozenset] = None
_PAGE_EXTS_CACHE: Optional[frozenset] = None


def register_optional_plugins() -> None:
    """注册可选的 Pillow 解码插件（pillow-heif / pillow-jxl-plugin）。

    幂等；插件缺失静默跳过（对应的扩展名会被 :func:`supported_image_exts`
    过滤掉，列表不会出现打不开的图片）。
    """
    global _PLUGINS_REGISTERED
    with _BACKEND_LOCK:
        if _PLUGINS_REGISTERED:
            return
        _PLUGINS_REGISTERED = True
    try:  # HEIC / HEIF
        from pillow_heif import register_heif_opener  # noqa: PLC0415

        register_heif_opener()
        _BACKEND_READY["heif"] = True
    except Exception:
        _BACKEND_READY["heif"] = False
    try:  # JPEG XL（不同发行版入口不同，逐个尝试）
        import pillow_jxl  # noqa: PLC0415

        register = getattr(pillow_jxl, "register_jxl_opener", None)
        if callable(register):
            register()
        _BACKEND_READY["jxl"] = True
    except Exception:
        _BACKEND_READY["jxl"] = False


def image_backend_ready(kind: str) -> bool:
    """可选图像后端是否可用（导入探测，进程内缓存）。"""
    register_optional_plugins()
    with _BACKEND_LOCK:
        cached = _BACKEND_READY.get(kind)
    if cached is not None:
        return bool(cached)
    ready = False
    if kind == "svg":
        ready = _module_ok("svglib.svg2rlg") and _module_ok("reportlab.graphics.renderPM")
    elif kind == "psd":
        ready = _module_ok("psd_tools")
    elif kind == "raw":
        ready = _module_ok("rawpy")
    elif kind in ("heif", "jxl"):
        ready = bool(_BACKEND_READY.get(kind))
    with _BACKEND_LOCK:
        _BACKEND_READY[kind] = ready
    return ready


def _module_ok(name: str) -> bool:
    try:
        __import__(name)
        return True
    except Exception:
        return False


def supported_image_exts() -> frozenset:
    """当前进程能真正解码的图片扩展名集合（基础 + 已装后端的异形格式）。

    结果进程内恒定（插件只注册一次），做缓存：容器构造会逐条目调用本函数。
    """
    global _SUPPORTED_CACHE
    register_optional_plugins()
    with _BACKEND_LOCK:
        cached = _SUPPORTED_CACHE
    if cached is not None:
        return cached
    exts = set(BASE_IMAGE_EXTS)
    for ext, kind in _EXOTIC_IMAGE_BACKENDS.items():
        if image_backend_ready(kind):
            exts.add(ext)
    result = frozenset(exts)
    with _BACKEND_LOCK:
        _SUPPORTED_CACHE = result
    return result


def page_image_exts() -> frozenset:
    """容器/图集**页**白名单：单图可解码集合 + TIFF。

    独立 ``.tif/.tiff`` 文件由 ``tiff`` 格式（多页适配器）打开，不在这里；
    但压缩包/文件夹里的 TIFF 页仍应作为一页图片（旧行为不能因 M5 丢页）。
    """
    global _PAGE_EXTS_CACHE
    with _BACKEND_LOCK:
        cached = _PAGE_EXTS_CACHE
    if cached is not None:
        return cached
    result = frozenset(set(supported_image_exts()) | {".tif", ".tiff"})
    with _BACKEND_LOCK:
        _PAGE_EXTS_CACHE = result
    return result


def looks_like_image_file(path) -> bool:
    """按文件头判断"这像个图片吗"（无扩展名 / 错扩展名的单图兜底）。

    只读前 4KB；能识别魔数的基础格式 + 已装后端的异形格式（reads 扩展名无法
    提供时，输出仍交给 :func:`decode_for_display` 的异形转换链）。
    """
    from pathlib import Path

    try:
        with open(Path(path), "rb") as fh:
            head = fh.read(4096)
    except OSError:
        return False
    if not head:
        return False
    if sniff_mime(head, "").startswith("image/"):
        return True
    # 常见异形格式魔数（与 decode/exotic 支持的后端对应）
    if len(head) >= 12 and head[4:8] == b"ftyp" and head[8:12] in (b"heic", b"heix", b"hevc", b"mif1", b"msf1", b"avif"):
        return True
    if head[:2] in (b"\xff\x0a",) or head[:12].endswith(b"JXL \r\n\x87\n"):
        return True
    if head[:4] == b"8BPS":
        return True
    if head[:4] in (b"II*\x00", b"MM\x00*") and head[8:12] in (b"CR\x02\x00", b"CR\x03\x00"):
        return True
    return False


def _convert_exotic(data: bytes, ext: str) -> Image.Image:
    """把 Pillow 打不开的异形图片转成 PIL Image（依赖可选后端，缺失即抛错）。"""
    e = (ext or "").lower()
    kind = _EXOTIC_IMAGE_BACKENDS.get(e)
    if kind == "svg":
        return _convert_svg(data)
    if kind == "psd":
        return _convert_psd(data)
    if kind == "raw":
        return _convert_raw(data)
    raise ImageDecodeError(f"没有可用的解码后端: {ext}")


def _convert_svg(data: bytes) -> Image.Image:
    try:
        from reportlab.graphics import renderPM  # noqa: PLC0415
        from svglib.svglib import svg2rlg  # noqa: PLC0415

        drawing = svg2rlg(io.BytesIO(data))
        if drawing is None:
            raise ImageDecodeError("SVG 无法解析")
        return renderPM.drawToPIL(drawing)
    except ImageDecodeError:
        raise
    except Exception as exc:
        try:  # cairosvg 备选
            import cairosvg  # noqa: PLC0415

            png = cairosvg.svg2png(bytestring=data)
            img = Image.open(io.BytesIO(png))
            img.load()
            return img
        except Exception:
            raise ImageDecodeError(f"SVG 解码失败: {exc}") from exc


def _convert_psd(data: bytes) -> Image.Image:
    try:
        from psd_tools import PSDImage  # noqa: PLC0415

        psd = PSDImage.open(io.BytesIO(data))
        img = psd.composite()
        if img is None:
            raise ImageDecodeError("PSD 没有合成图层")
        return _to_pil(img)
    except ImageDecodeError:
        raise
    except Exception as exc:
        raise ImageDecodeError(f"PSD 解码失败: {exc}") from exc


def _convert_raw(data: bytes) -> Image.Image:
    try:
        import rawpy  # noqa: PLC0415

        with rawpy.imread(io.BytesIO(data)) as raw:
            rgb = raw.postprocess(use_camera_wb=True, output_bps=8)
        return Image.fromarray(rgb)
    except Exception as exc:
        raise ImageDecodeError(f"RAW 解码失败: {exc}") from exc


def _to_pil(obj) -> Image.Image:
    """psd-tools 的 composite 结果（PIL 或 numpy）统一成 PIL Image。"""
    if isinstance(obj, Image.Image):
        return obj
    try:
        return Image.fromarray(obj)  # numpy ndarray
    except Exception as exc:  # pragma: no cover
        raise ImageDecodeError(f"无法转换解码结果: {exc}") from exc


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
        ".heic": "image/heic",
        ".heif": "image/heif",
        ".jxl": "image/jxl",
        ".svg": "image/svg+xml",
        ".psd": "image/vnd.adobe.photoshop",
        ".tga": "image/x-tga",
        ".pcx": "image/x-pcx",
        ".ico": "image/x-icon",
        ".dds": "image/vnd-ms.dds",
        ".pnm": "image/x-portable-anymap",
        ".ppm": "image/x-portable-pixmap",
        ".pgm": "image/x-portable-graymap",
        ".pbm": "image/x-portable-bitmap",
        ".xbm": "image/x-xbitmap",
        ".xpm": "image/x-xpixmap",
        ".cr2": "image/x-canon-cr2",
        ".cr3": "image/x-canon-cr3",
        ".nef": "image/x-nikon-nef",
        ".arw": "image/x-sony-arw",
        ".dng": "image/x-adobe-dng",
        ".orf": "image/x-olympus-orf",
        ".raf": "image/x-fuji-raf",
        ".rw2": "image/x-panasonic-rw2",
        # 音频 / 视频（媒体流式端点用）
        ".mp3": "audio/mpeg",
        ".m4a": "audio/mp4",
        ".m4b": "audio/mp4",
        ".aac": "audio/aac",
        ".flac": "audio/flac",
        ".ogg": "audio/ogg",
        ".oga": "audio/ogg",
        ".opus": "audio/opus",
        ".wav": "audio/wav",
        ".wma": "audio/x-ms-wma",
        ".mp4": "video/mp4",
        ".m4v": "video/mp4",
        ".mkv": "video/x-matroska",
        ".webm": "video/webm",
        ".mov": "video/quicktime",
        ".avi": "video/x-msvideo",
        ".3gp": "video/3gpp",
        ".ttf": "font/ttf",
        ".otf": "font/otf",
        ".ttc": "font/collection",
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
        if data[:4] == b"\x00\x00\x01\x00":
            return "image/x-icon"
        if data[:4] == b"DDS ":
            return "image/vnd-ms.dds"
        if data[:4] == b"8BPS":
            return "image/vnd.adobe.photoshop"
        if data[4:8] == b"ftyp":
            brand = data[8:12]
            if brand in (b"heic", b"heix", b"hevc", b"hevx"):
                return "image/heic"
            if brand in (b"mif1", b"msf1", b"heif"):
                return "image/heif"
            if brand in (b"avif", b"avis"):
                return "image/avif"
        if data[:12].endswith(b"JXL \r\n\x87\n") or data[:2] == b"\xff\x0a":
            return "image/jxl"
    if len(data) >= 2 and data[:2] in (b"P1", b"P2", b"P3", b"P4", b"P5", b"P6"):
        return "image/x-portable-anymap"
    return mime_for_ext(fallback_ext)


def open_image(data: bytes) -> Image.Image:
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
        return img
    except (UnidentifiedImageError, OSError, ValueError) as exc:
        raise ImageDecodeError(str(exc)) from exc


def probe_size(data: bytes, ext: str = "") -> Optional[Tuple[int, int]]:
    """只读文件头取得 (宽, 高)，不解码像素。

    必须与 :func:`decode_for_display` 的几何一致：后者会做 EXIF 方向校正
    （``ImageOps.exif_transpose``），旋转 90° 的图片宽高会互换。这里同样按
    Orientation 标签交换，否则"分页用的宽高"与"实际画出来的宽高"不一致，
    会出现图片被拉伸、条漫切片错位等问题。

    ``ext``：Pillow 打不开的异形格式（SVG/PSD/RAW）需要扩展名走转换链。
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
        if ext and _EXOTIC_IMAGE_BACKENDS.get((ext or "").lower()):
            try:
                return tuple(_convert_exotic(data, ext).size)  # type: ignore[return-value]
            except Exception:
                return None
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
    ext: str = "",
) -> Image.Image:
    """解码图片并校正方向；若超出目标尺寸则按需缩放。

    - ``target_w`` 为期望输出宽度（像素）；
    - JPEG 优先 draft 降采样；
    - 超过 ``max_source_pixels`` 时先 draft，仍超限则抛错（调用方返回 PARSE_FAILED）；
    - ``ext``：Pillow 打不开的异形格式（HEIC/JXL/SVG/PSD/RAW）交给可选后端
      （缺后端时抛 :class:`ImageDecodeError`，由调用方映射 PARSE_FAILED）。
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
        if ext and _EXOTIC_IMAGE_BACKENDS.get((ext or "").lower()):
            img = _convert_exotic(data, ext)
        else:
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
