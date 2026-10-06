"""字体发现、缓存与字符宽度（设计 4.5 / 4.1）。

- 字体来源：``render.fonts_dir``（内置 Noto Sans SC 放置处）+ 系统字体目录兜底；
- **字符宽度表**：CJK / 全角标点按全角宽度（= font_size）处理，O(1) 查表；
  ASCII / 拉丁范围按 token 调用 FreeType 度量并做 memo 缓存，避免逐字 FT 调用；
- 粗体 / 斜体不切换字体文件（保证分页宽度与渲染宽度一致），由渲染层用二次描边
  与偏移模拟；
- 找不到任何可用字体时退化为 Pillow 内置位图字体（中文会显示为方框，启动时告警）。
"""

from __future__ import annotations

import platform
import threading
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

from ..config import AppConfig

FONT_EXTS = (".ttf", ".otf", ".ttc", ".otc")

# 名称里含这些片段的字体优先用于中文渲染
CJK_HINTS = (
    "notosanssc",
    "noto sans sc",
    "notoserifsc",
    "sourcehansans",
    "source han",
    "思源",
    "sarasa",
    "lxgw",
    "msyh",
    "microsoft yahei",
    "simhei",
    "simsun",
    "dengxian",
    "pingfang",
    "hiragino",
    "songti",
    "kaiti",
    "fangsong",
    "wqy",
    "wenquanyi",
    "droidsansfallback",
    "arial unicode",
    "unifont",
)

# Windows / macOS / Linux 常见系统字体目录
_SYSTEM_FONT_DIRS = {
    "Windows": [Path(r"C:\Windows\Fonts"), Path.home() / "AppData/Local/Microsoft/Windows/Fonts"],
    "Darwin": [
        Path("/System/Library/Fonts"),
        Path("/Library/Fonts"),
        Path.home() / "Library/Fonts",
    ],
    "Linux": [
        Path("/usr/share/fonts"),
        Path("/usr/local/share/fonts"),
        Path.home() / ".fonts",
        Path.home() / ".local/share/fonts",
    ],
}

MAX_FONT_FILES = 600
_MEASURE_CACHE_MAX = 200_000
# 字号 → FreeType 字体对象缓存上限
_FONT_CACHE_MAX = 64
# 文本宽度缓存：只缓存短串，避免缓存本身吃掉内存
_TEXT_CACHE_MAX_LEN = 64
_TEXT_CACHE_MAX_ENTRIES = 50_000


def is_wide_char(ch: str) -> bool:
    """CJK / 全角字符：按全角宽度（= font_size）处理（设计 4.5）。"""
    code = ord(ch)
    if code < 0x1100:
        return False
    return (
        0x1100 <= code <= 0x115F
        or 0x2E80 <= code <= 0x303E
        or 0x3041 <= code <= 0x33FF
        or 0x3400 <= code <= 0x4DBF
        or 0x4E00 <= code <= 0x9FFF
        or 0xA000 <= code <= 0xA4CF
        or 0xAC00 <= code <= 0xD7A3
        or 0xF900 <= code <= 0xFAFF
        or 0xFE10 <= code <= 0xFE19
        or 0xFE30 <= code <= 0xFE6F
        or 0xFF00 <= code <= 0xFF60
        or 0xFFE0 <= code <= 0xFFE6
        or 0x1F300 <= code <= 0x1FAFF
        or 0x20000 <= code <= 0x3FFFD
    )


@dataclass(frozen=True)
class FontFace:
    id: str
    name: str
    path: Path
    index: int = 0
    builtin: bool = False

    def to_info(self, default: bool = False) -> Dict[str, object]:
        return {
            "id": self.id,
            "name": self.name,
            "file_name": self.path.name,
            "default": default,
            "builtin": self.builtin,
        }


class FontManager:
    """字体发现 + 度量缓存。

    线程安全：渲染在线程池里并发执行（``asyncio.to_thread``），下面这几个缓存
    会被多个线程同时读写。``OrderedDict.move_to_end`` + ``popitem`` 的组合在没有
    锁时可能撞上"检查时非空、弹出时已被别人弹空"而抛 ``KeyError``（表现为 500）。
    所有缓存操作统一走 :attr:`_cache_lock`。
    """

    def __init__(self, config: AppConfig) -> None:
        self.config = config
        self._faces: "OrderedDict[str, FontFace]" = OrderedDict()
        self._font_cache: "OrderedDict[Tuple[str, int], ImageFont.FreeTypeFont]" = OrderedDict()
        # 文本宽度缓存：(字体@字号@间距, 文本) → 宽度
        self._token_cache: "OrderedDict[Tuple[str, str], float]" = OrderedDict()
        self._char_cache: Dict[Tuple[str, int, str], float] = {}
        self._cjk_cache: Dict[Any, bool] = {}
        self._cache_lock = threading.RLock()
        self._default_id: Optional[str] = None
        self._missing_font = False
        self.discover()

    # ------------------------------------------------------------ 发现
    def discover(self) -> None:
        self._faces.clear()
        self._default_id = None
        self._missing_font = False
        with self._cache_lock:
            self._cjk_cache.clear()

        for face in self._scan_dir(Path(self.config.fonts_dir), builtin=True):
            self._register(face)
        for directory in self._system_dirs():
            for face in self._scan_dir(directory, builtin=False):
                self._register(face)

        if not self._faces:
            self._missing_font = True
            return

        want = (self.config.render.default_font or "").strip().lower()
        if want:
            for fid, face in self._faces.items():
                if fid.lower() == want or face.name.lower() == want:
                    self._default_id = fid
                    break
        if self._default_id is None:
            # 1) 内置字体目录里的中文字体（用户按 README 放了 Noto Sans SC 就走这里）
            builtin = [f for f in self._faces.values() if f.builtin]
            for face in builtin:
                if self._can_render_cjk(face):
                    self._default_id = face.id
                    break
            if self._default_id is None and builtin:
                self._default_id = builtin[0].id
        if self._default_id is None:
            # 2) 任何真的画得出中文的字体
            for face in self._faces.values():
                if self._can_render_cjk(face):
                    self._default_id = face.id
                    break
        if self._default_id is None:
            # 3) 字形探测全军覆没（异常/不支持的字体格式）时的兜底：
            #    按字体名猜。**不能**直接退回"列表里第一个"—— 那在 Windows 上
            #    会选中 Arial，整本中文渲染成方框，而且只有一个启动日志能提示。
            for face in self._faces.values():
                if _name_suggests_cjk(face):
                    self._default_id = face.id
                    break
        if self._default_id is None:
            self._default_id = next(iter(self._faces))

    def _system_dirs(self) -> List[Path]:
        system = platform.system()
        dirs = list(_SYSTEM_FONT_DIRS.get(system, []))
        if system not in _SYSTEM_FONT_DIRS:
            dirs.extend(_SYSTEM_FONT_DIRS["Linux"])
        return [d for d in dirs if d and d.exists()]

    def _scan_dir(self, directory: Path, *, builtin: bool) -> Iterable[FontFace]:
        if not directory or not directory.exists():
            return []
        out: List[FontFace] = []
        try:
            entries = sorted(directory.rglob("*"))
        except OSError:
            return []
        count = 0
        for path in entries:
            if count >= MAX_FONT_FILES:
                break
            try:
                if not path.is_file() or path.suffix.lower() not in FONT_EXTS:
                    continue
            except OSError:
                continue
            count += 1
            fid = _face_id(path)
            if not fid:
                continue
            out.append(
                FontFace(id=fid, name=path.stem, path=path, index=0, builtin=builtin)
            )
        return out

    def _register(self, face: FontFace) -> None:
        if face.id in self._faces:
            # 内置字体优先于系统字体
            if not face.builtin:
                return
        self._faces[face.id] = face

    # ------------------------------------------------------------ 查询
    @property
    def missing_font(self) -> bool:
        """没有任何可用字体文件（中文会显示为方框）。"""
        return self._missing_font

    @property
    def default_id(self) -> str:
        return self._default_id or "default"

    def has_cjk_font(self) -> bool:
        if self._missing_font:
            return False
        face = self._faces.get(self.default_id)
        return bool(face and self._can_render_cjk(face))

    def list_faces(self) -> List[FontFace]:
        return list(self._faces.values())

    def list_infos(self) -> List[Dict[str, object]]:
        default = self.default_id
        return [f.to_info(default=(f.id == default)) for f in self._faces.values()]

    def get(self, family: Optional[str]) -> Optional[FontFace]:
        fam = (family or "").strip()
        if not fam or fam.lower() == "auto":
            return self._faces.get(self.default_id)
        if fam in self._faces:
            return self._faces[fam]
        low = fam.lower()
        for fid, face in self._faces.items():
            if fid.lower() == low or face.name.lower() == low:
                return face
        for fid, face in self._faces.items():
            if low in fid.lower():
                return face
        return self._faces.get(self.default_id)

    def resolve_id(self, family: Optional[str]) -> str:
        face = self.get(family)
        return face.id if face else "default"

    # ------------------------------------------------------------ 度量
    def font(self, family: Optional[str], size_px: int) -> ImageFont.FreeTypeFont:
        """取得指定字号的字体对象（粗体不换文件，见模块 docstring）。"""
        fam = self.resolve_id(family)
        size = max(4, int(size_px))
        key = (fam, size)
        with self._cache_lock:
            cached = self._font_cache.get(key)
            if cached is not None:
                self._font_cache.move_to_end(key)
                return cached

        font = None
        face = self._faces.get(fam)
        if face is not None:
            for index in (face.index, 0, 1, 2):
                try:
                    font = ImageFont.truetype(str(face.path), size, index=index)
                    break
                except Exception:
                    continue
        if font is None:
            self._missing_font = True
            font = _default_font(size)

        with self._cache_lock:
            self._font_cache[key] = font
            self._font_cache.move_to_end(key)
            while len(self._font_cache) > _FONT_CACHE_MAX:
                self._font_cache.popitem(last=False)
        return font

    def char_width(self, ch: str, family: Optional[str], size_px: int, spacing: float = 0.0) -> float:
        if not ch:
            return 0.0
        if is_wide_char(ch):
            return float(size_px) + spacing
        fam = self.resolve_id(family)
        key = (fam, int(size_px), ch)
        with self._cache_lock:
            cached = self._char_cache.get(key)
        if cached is None:
            try:
                cached = float(self.font(family, size_px).getlength(ch))
            except Exception:
                cached = float(size_px) * 0.5
            with self._cache_lock:
                if len(self._char_cache) > _MEASURE_CACHE_MAX:
                    self._char_cache.clear()
                self._char_cache[key] = cached
        return cached + spacing

    def text_width(
        self, text: str, family: Optional[str], size_px: int, spacing: float = 0.0, *, indent_chars: int = 0
    ) -> float:
        """文本宽度（供分页/渲染共用，保证两者一致）。

        缓存键使用**文本本身**（长度 ≤ 64 的短串才缓存）：用 ``hash(text)`` 会因 32/64 位
        截断产生碰撞，一旦碰撞就会用错宽度，直接破坏折行结果。
        """
        if not text:
            base = 0.0
        else:
            fam = self.resolve_id(family)
            size = int(size_px)
            cacheable = len(text) <= _TEXT_CACHE_MAX_LEN
            cache_key = (f"{fam}@{size}@{round(spacing, 3)}", text) if cacheable else None
            cached: Optional[float] = None
            if cache_key is not None:
                with self._cache_lock:
                    cached = self._token_cache.get(cache_key)
                    if cached is not None:
                        self._token_cache.move_to_end(cache_key)
            if cached is not None:
                base = cached
            else:
                base = self._measure(text, family, size, spacing)
                if cache_key is not None:
                    with self._cache_lock:
                        self._token_cache[cache_key] = base
                        while len(self._token_cache) > _TEXT_CACHE_MAX_ENTRIES:
                            self._token_cache.popitem(last=False)
        if indent_chars:
            base += float(indent_chars) * (float(size_px) + spacing)
        return base

    def _measure(self, text: str, family: Optional[str], size_px: int, spacing: float) -> float:
        """宽字符走全角表，其余交给 FreeType 一次度量整段。"""
        if not any(is_wide_char(ch) for ch in text):
            try:
                return float(self.font(family, size_px).getlength(text)) + spacing * len(text)
            except Exception:
                pass
        total = 0.0
        buffer: List[str] = []

        def flush() -> None:
            nonlocal total
            if buffer:
                chunk = "".join(buffer)
                try:
                    total += float(self.font(family, size_px).getlength(chunk))
                except Exception:
                    total += float(size_px) * 0.5 * len(chunk)
                buffer.clear()

        for ch in text:
            if is_wide_char(ch):
                flush()
                total += float(size_px)
            else:
                buffer.append(ch)
        flush()
        return total + spacing * len(text)

    def can_render(self, family: Optional[str], text: str) -> bool:
        face = self.get(family)
        if face is None:
            return False
        return self._can_render_cjk(face, sample=text or "中")

    def _can_render_cjk(self, face: FontFace, sample: str = "中") -> bool:
        """该字体是否真的能画出中文字形（而不是 .notdef 方框）。

        实现必须**光栅化后比较像素**：Pillow 12 的 ``font.getmask()`` 返回
        ``ImagingCore``，它没有 ``tobytes()`` —— 旧实现写成 ``getmask(...).tobytes()``，
        每一次调用都抛 AttributeError，又被下面的 ``except Exception`` 吞掉，
        于是本函数**永远返回 False**。后果是 ``discover()`` 里所有"优先选中文
        字体"的分支全部失效，默认字体退化成按文件名排序的第一个（Windows 上是
        Arial）—— 整本中文小说渲染出来全是方框。

        这里改用 ImageDraw 渲染两个字符再比像素，跨 Pillow 版本稳定，也顺带把
        "字形为空"（空格之类）判掉。
        """
        key = (face.id, sample)
        with self._cache_lock:
            cached = self._cjk_cache.get(key)
        if cached is not None:
            return cached
        result = _rasterize_glyph(face, sample) not in (None, _NOTDEF_SENTINEL)
        with self._cache_lock:
            self._cjk_cache[key] = result
        return result


def _face_id(path: Path) -> str:
    stem = path.stem.strip()
    if not stem:
        return ""
    return stem


# 用一个"字体必然没有、且会落到 .notdef"的私有区码位作为方框基准
_NOTDEF_PROBE = "\ue123"
_NOTDEF_SENTINEL = "notdef"
_GLYPH_PROBE_SIZE = 32


def _rasterize_glyph(face: FontFace, ch: str) -> Optional[bytes]:
    """把单个字符画到小画布上并返回像素；无法渲染时返回 None。

    返回 ``_NOTDEF_SENTINEL`` 表示"画出来的是 .notdef 方框"（即字体没有这个字形）。
    """
    try:
        font = ImageFont.truetype(str(face.path), _GLYPH_PROBE_SIZE, index=face.index)
    except Exception:
        return None
    size = _GLYPH_PROBE_SIZE * 2
    try:
        canvas = Image.new("L", (size, size), 0)
        ImageDraw.Draw(canvas).text((2, 2), ch, font=font, fill=255)
        ink = canvas.tobytes()
    except Exception:
        return None
    if not any(ink):
        return None  # 完全没有墨迹：视为不可用
    try:
        notdef_canvas = Image.new("L", (size, size), 0)
        ImageDraw.Draw(notdef_canvas).text((2, 2), _NOTDEF_PROBE, font=font, fill=255)
        if ink == notdef_canvas.tobytes():
            return _NOTDEF_SENTINEL
    except Exception:
        pass
    return ink


def _name_suggests_cjk(face: FontFace) -> bool:
    """按字体文件名判断"大概率含中文"（字形探测失败时的兜底信号）。"""
    haystack = f"{face.id} {face.name} {face.path.name}".lower()
    return any(hint in haystack for hint in CJK_HINTS)


def _default_font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.load_default(size=size)
