"""漫画容器与文件夹图集适配器（设计 3.6 / 7.4 / 7.5）。

容器统一抽象：CBZ/ZIP、CBR/RAR、CB7/7z、CBT/TAR 与"图片文件夹即书"。

要点：
- 只收录图片扩展名白名单，按自然排序（数字感知，忽略大小写）；
- 跳过 ``__MACOSX/``、隐藏条目、``Thumbs.db`` 等（可配置）；
- ZIP 按条目随机读取（不解压整包）；RAR/7z 需系统解压后端，缺失时给出明确原因；
- 条目名编码兜底：非 UTF-8 条目按 cp437 解码后启发式尝试 GBK / Shift-JIS；
- 安全：条目名规范化（防 zip-slip）、单条目与总解压大小上限（防 zip 炸弹）。
- 条漫 strip：按视口高度切片，锚点 ``(0, 源图索引, 纵向偏移)``。
"""

from __future__ import annotations

import os
import posixpath
import tarfile
import threading
import zipfile
from abc import ABC, abstractmethod
from fnmatch import fnmatch
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from ..config import IMAGE_EXT
from ..util import imaging
from ..util.natural import natural_key
from ..util.paths import ext_of, stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    AssetNotFoundError,
    BookRef,
    FixedAdapter,
    ParseError,
    StripPage,
    UnsupportedFormatError,
)

_ZIP_EXTS = {".cbz", ".zip"}
_RAR_EXTS = {".cbr", ".rar"}
_SEVEN_EXTS = {".cb7", ".7z"}
_TAR_EXTS = {".cbt", ".tar"}


# ------------------------------------------------------------------ 工具
def is_image_name(name: str) -> bool:
    return ext_of(name) in set(IMAGE_EXT)


def decode_zip_name(info: zipfile.ZipInfo) -> str:
    """ZIP 条目名编码兜底（设计 7.4 / 风险 18）。"""
    if info.flag_bits & 0x800:
        return info.filename
    try:
        raw = info.filename.encode("cp437", errors="replace")
    except Exception:
        return info.filename
    for enc in ("utf-8", "gbk", "big5", "shift_jis"):
        try:
            text = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        if _plausible(text):
            return text
    return info.filename


def _plausible(text: str) -> bool:
    if not text or "\ufffd" in text:
        return False
    bad = sum(1 for ch in text if ord(ch) < 32 and ch not in "\t")
    return bad == 0


def should_skip_entry(name: str, patterns: Sequence[str]) -> bool:
    """通用跳过规则：隐藏段、配置中的 glob 模式。

    "隐藏"的判定与 :func:`app.util.paths.is_hidden_name` 保持一致：``.`` 与 ``..``
    是路径语义符而不是隐藏文件名。早期版本把任意以 ``.`` 开头的段都当隐藏，于是
    ``./dir/01.png`` 这种条目被整包丢掉 —— 而 ``zip`` / ``7z`` / ``bsdtar`` 生成的
    压缩包确实会带 ``./`` 前缀，结果是整本书"没有可显示的内容"。

    glob 匹配针对**规范化后**的路径：默认规则里的 ``*/.*`` 本意是"隐藏目录里的文件"，
    但直接拿原始名去匹配会把 ``foo/./bar.png`` 也一起误杀（``fnmatch`` 看到 "/."）。
    """
    norm = name.replace("\\", "/").strip("/")
    if not norm:
        return True
    segments = norm.split("/")
    base = segments[-1]
    for seg in segments[:-1]:
        if seg.startswith(".") and seg not in (".", ".."):
            return True
    if base.startswith(".") and base not in (".", ".."):
        return True
    canonical = posixpath.normpath(norm)
    for pat in patterns or ():
        p = str(pat).strip()
        if not p:
            continue
        if fnmatch(canonical, p) or fnmatch(base, p):
            return True
        if p.startswith("*/") and fnmatch(base, p[2:]):
            return True
    return False


def is_unsafe_entry_name(norm: str) -> bool:
    """规范化后的条目名是否越出容器根（zip-slip / 绝对路径 / 盘符 / ADS）。

    所有容器都必须用同一个判据：早期只有 ZIP 检查了盘符冒号，TAR/RAR/7z 会放行
    ``C:/evil.png`` 这类条目名（当前不会变成文件系统路径，但没有任何理由放宽）。
    """
    if not norm or norm in (".", ".."):
        return True
    if norm.startswith("../") or norm.startswith("/"):
        return True
    if ":" in norm.split("/")[0]:
        return True
    return False


def _sort_names(names: Sequence[str]) -> List[str]:
    return sorted(names, key=natural_key)


# ------------------------------------------------------------------ 容器
class ComicContainer(ABC):
    """漫画容器：只暴露"图片条目名列表 + 按名读取"。"""

    def __init__(self, env: AdapterEnv) -> None:
        self.env = env
        self.patterns = list(env.config.comic.skip_entries)
        self.max_entry_bytes = int(env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024
        self.max_total_bytes = int(env.config.limits.max_archive_total_mb or 0) * 1024 * 1024

    @property
    @abstractmethod
    def names(self) -> List[str]:
        """图片条目名（已自然排序）。"""

    @abstractmethod
    def read(self, name: str) -> bytes:
        """读取条目字节。"""

    def size_of(self, name: str) -> int:
        return len(self.read(name))

    def close(self) -> None:
        return None

    def __enter__(self) -> "ComicContainer":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _guard_entry(self, name: str, size: int) -> None:
        if self.max_entry_bytes and size > self.max_entry_bytes:
            raise ParseError(f"压缩包条目过大（{size / 1048576:.1f}MB）: {name}")

    def _guard_total(self, total: int) -> None:
        if self.max_total_bytes and total > self.max_total_bytes:
            raise ParseError(f"压缩包解压总量超过上限（{total / 1048576:.1f}MB），已拒绝")

    def _read_stream(self, fh, name: str) -> bytes:
        """流式读取并强制单条目上限。

        只信压缩包头里声明的尺寸是不够的（声明值可能远小于实际解压量），
        因此这里最多读 ``limit + 1`` 字节，超限立即失败，避免内存被撑爆。
        """
        limit = self.max_entry_bytes
        if not limit:
            return fh.read()
        data = fh.read(limit + 1)
        if len(data) > limit:
            raise ParseError(
                f"压缩包条目解压后超过上限（{limit // 1048576}MB）: {name}"
            )
        return data

    def _check_size(self, name: str, payload: bytes) -> bytes:
        """对无法流式限制的容器（7z）做读后校验。"""
        if self.max_entry_bytes and len(payload) > self.max_entry_bytes:
            raise ParseError(
                f"压缩包条目解压后超过上限（{self.max_entry_bytes // 1048576}MB）: {name}"
            )
        return payload


class ZipComicContainer(ComicContainer):
    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            self._zip = zipfile.ZipFile(path)
        except (OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"ZIP 容器无法打开: {exc}") from exc
        mapping: Dict[str, str] = {}
        sizes: Dict[str, int] = {}
        total = 0
        for info in self._zip.infolist():
            if info.is_dir():
                continue
            name = decode_zip_name(info).replace("\\", "/")
            if should_skip_entry(name, self.patterns):
                continue
            if not is_image_name(name):
                continue
            # zip-slip / 条目名规范化
            norm = posixpath.normpath(name)
            if is_unsafe_entry_name(norm):
                continue
            mapping[norm] = info.filename
            # 尺寸也按规范化后的键记录，否则 _guard_entry 会因查不到而失效
            sizes[norm] = int(info.file_size or 0)
            total += sizes[norm]
        self._guard_total(total)
        self._map = mapping
        self._sizes = sizes
        self._names = _sort_names(list(mapping.keys()))

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        real = self._map.get(key)
        if real is None:
            raise AssetNotFoundError(f"压缩包内不存在条目: {name}")
        self._guard_entry(key, self.size_of(key))
        try:
            with self._zip.open(real) as fh:
                return self._read_stream(fh, key)
        except ParseError:
            raise
        except (KeyError, OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"条目读取失败 {name}: {exc}") from exc

    def close(self) -> None:
        try:
            self._zip.close()
        except Exception:
            pass


def rar_backend_tool(rarfile_module) -> Optional[object]:
    """探测 RAR 解压后端，返回工具对象或 None。

    ``rarfile`` 的行为在版本间变过：老版本 ``tool_setup()`` 找不到工具时返回
    ``None``，4.4+ 直接抛 ``RarCannotExec``。只判断返回值会漏掉后者 ——
    异常一路冒到通用处理器就变成 500，而契约要求的是 415 + "需安装 unrar"。
    """
    try:
        return rarfile_module.tool_setup()
    except Exception:
        return None


class RarComicContainer(ComicContainer):
    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            import rarfile  # noqa: PLC0415
        except Exception as exc:  # pragma: no cover
            raise UnsupportedFormatError("CBR/RAR 支持需要 rarfile", reason="缺少 rarfile") from exc
        self._rarfile = rarfile
        if rar_backend_tool(rarfile) is None:
            raise UnsupportedFormatError(
                "CBR/RAR 需要系统解压后端（unrar / bsdtar），请安装后重试",
                reason="需安装 unrar",
            )
        try:
            self._rf = rarfile.RarFile(str(path))
        except Exception as exc:
            raise ParseError(f"RAR 容器无法打开: {exc}") from exc
        names: List[str] = []
        sizes: Dict[str, int] = {}
        mapping: Dict[str, str] = {}
        total = 0
        for info in self._rf.infolist():
            if getattr(info, "is_dir", lambda: False)():
                continue
            raw = str(info.filename).replace("\\", "/")
            if should_skip_entry(raw, self.patterns) or not is_image_name(raw):
                continue
            norm = posixpath.normpath(raw)
            if is_unsafe_entry_name(norm):
                continue
            names.append(norm)
            sizes[norm] = int(getattr(info, "file_size", 0) or 0)
            # RAR 的条目名可能带 "./" 前缀等需要规范化的写法：必须记住
            # 规范化名 → 容器内真实名，否则列表正常但每一页读取都失败
            mapping[norm] = str(info.filename)
            total += sizes[norm]
        self._guard_total(total)
        self._names = _sort_names(names)
        self._sizes = sizes
        self._map = mapping

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        self._guard_entry(key, self.size_of(key))
        raw = self._map.get(key)
        if raw is None:
            raise AssetNotFoundError(f"RAR 内不存在条目: {name}")
        try:
            with self._rf.open(raw) as fh:
                # 与 ZIP/TAR 一致地流式限流：只信包头声明的尺寸会被 zip 炸弹绕过
                return self._read_stream(fh, key)
        except ParseError:
            raise
        except Exception as exc:
            raise ParseError(f"RAR 条目读取失败 {name}: {exc}") from exc

    def close(self) -> None:
        try:
            self._rf.close()
        except Exception:
            pass


class SevenZipComicContainer(ComicContainer):
    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            import py7zr  # noqa: PLC0415
        except Exception as exc:  # pragma: no cover
            raise UnsupportedFormatError("CB7/7z 支持需要 py7zr", reason="缺少 py7zr") from exc
        self._py7zr = py7zr
        try:
            self._arc = py7zr.SevenZipFile(str(path), "r")
            infos = self._arc.list()
        except Exception as exc:
            raise ParseError(f"7z 容器无法打开: {exc}") from exc
        names: List[str] = []
        sizes: Dict[str, int] = {}
        mapping: Dict[str, str] = {}
        total = 0
        for info in infos:
            if getattr(info, "is_directory", False):
                continue
            raw = str(info.filename).replace("\\", "/")
            if should_skip_entry(raw, self.patterns) or not is_image_name(raw):
                continue
            norm = posixpath.normpath(raw)
            if is_unsafe_entry_name(norm):
                continue
            names.append(norm)
            sizes[norm] = int(getattr(info, "uncompressed", 0) or 0)
            mapping[norm] = raw
            total += sizes[norm]
        self._guard_total(total)
        self._names = _sort_names(names)
        self._sizes = sizes
        self._map = mapping
        self._lock = threading.Lock()

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        self._guard_entry(key, self.size_of(key))
        raw = self._map.get(key, key)
        with self._lock:
            try:
                self._arc.reset()
            except Exception:
                pass
            try:
                result = self._arc.read(targets=[raw])
            except Exception as exc:
                raise ParseError(f"7z 条目读取失败 {name}: {exc}") from exc
        data = result.get(raw) if isinstance(result, dict) else None
        if data is None:
            for k, v in (result or {}).items():
                if posixpath.normpath(str(k)) == key:
                    data = v
                    break
        if data is None:
            raise AssetNotFoundError(f"7z 内不存在条目: {name}")
        payload = data.read() if hasattr(data, "read") else bytes(data)
        # 7z 无法按条目流式限流，只能读后校验（声明尺寸已在 _guard_entry 校验过一次）
        return self._check_size(key, payload)

    def close(self) -> None:
        try:
            self._arc.close()
        except Exception:
            pass


class TarComicContainer(ComicContainer):
    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            self._tar = tarfile.open(str(path), "r:*")
        except Exception as exc:
            raise ParseError(f"TAR 容器无法打开: {exc}") from exc
        names: List[str] = []
        sizes: Dict[str, int] = {}
        mapping: Dict[str, str] = {}
        total = 0
        for member in self._tar.getmembers():
            if not member.isfile():
                continue
            raw = str(member.name).replace("\\", "/")
            if should_skip_entry(raw, self.patterns) or not is_image_name(raw):
                continue
            norm = posixpath.normpath(raw)
            if is_unsafe_entry_name(norm):
                continue
            names.append(norm)
            sizes[norm] = int(member.size or 0)
            mapping[norm] = raw
            total += sizes[norm]
        self._guard_total(total)
        self._names = _sort_names(names)
        self._sizes = sizes
        self._map = mapping

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        self._guard_entry(key, self.size_of(key))
        raw = self._map.get(key, key)
        try:
            fh = self._tar.extractfile(raw)
        except KeyError:
            fh = None
        if fh is None:
            raise AssetNotFoundError(f"TAR 内不存在条目: {name}")
        with fh:
            return self._read_stream(fh, key)

    def close(self) -> None:
        try:
            self._tar.close()
        except Exception:
            pass


class FolderComicContainer(ComicContainer):
    """图片文件夹即书（含"作为图集打开"，设计 7.5）。

    也接受**单个图片文件**：此时以它的父目录为根、文件名作为唯一条目。
    ``GalleryService.register`` 允许把单张图片注册成"单页图集"（设计 7.5 /
    浏览页的"打开为图集"入口），而 ``open_container`` 以前只认目录与压缩包，
    于是注册成功（200）之后每一次 /meta、/layout、/pages 都是 415。
    """

    def __init__(self, env: AdapterEnv, path: Path, recursive: bool = False) -> None:
        super().__init__(env)
        source = Path(path)
        if source.is_file():
            self.root = source.parent
            self._single: Optional[str] = source.name
            self.recursive = False
        else:
            self.root = source
            self._single = None
            self.recursive = recursive
        self._names = self._scan()
        self._sizes = {n: self._safe_size(n) for n in self._names}

    def _safe_size(self, name: str) -> int:
        """扫描与 stat 之间文件可能被删除/改名，这里不能让 OSError 冒出来。"""
        try:
            return int(os.path.getsize(self.root / name))
        except OSError:
            return 0

    def _scan(self) -> List[str]:
        out: List[str] = []
        if self._single is not None:
            if not should_skip_entry(self._single, self.patterns) and is_image_name(self._single):
                return [self._single]
            return out
        if not self.root.is_dir():
            return out
        if self.recursive:
            for dirpath, dirnames, filenames in os.walk(self.root, followlinks=False):
                dirnames[:] = [d for d in dirnames if not d.startswith(".")]
                rel_dir = os.path.relpath(dirpath, self.root)
                for fn in filenames:
                    full = os.path.join(dirpath, fn)
                    # 符号链接不计入页集合：read() 会因越界而拒绝，否则页数虚高且必然加载失败
                    if os.path.islink(full):
                        continue
                    rel = fn if rel_dir in (".", "") else f"{rel_dir}/{fn}"
                    rel = rel.replace("\\", "/")
                    if should_skip_entry(rel, self.patterns) or not is_image_name(rel):
                        continue
                    out.append(rel)
        else:
            try:
                entries = list(os.scandir(self.root))
            except OSError as exc:
                raise ParseError(f"目录无法读取: {exc}") from exc
            for entry in entries:
                try:
                    if entry.is_symlink() or not entry.is_file():
                        continue
                except OSError:
                    continue
                if should_skip_entry(entry.name, self.patterns) or not is_image_name(entry.name):
                    continue
                out.append(entry.name)
        return _sort_names(out)

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        target = (self.root / key).resolve()
        try:
            target.relative_to(self.root.resolve())
        except ValueError as exc:
            raise AssetNotFoundError(f"非法条目: {name}") from exc
        try:
            return target.read_bytes()
        except OSError as exc:
            raise ParseError(f"图片读取失败 {name}: {exc}") from exc


def open_container(env: AdapterEnv, path: Path, *, recursive: bool = False) -> ComicContainer:
    """按扩展名选择容器；目录 / 单张图片 → 图集容器。"""
    p = Path(path)
    if p.is_dir():
        return FolderComicContainer(env, p, recursive=recursive)
    if is_image_name(p.name):
        # 单张图片 = 单页图集（设计 7.5）
        return FolderComicContainer(env, p, recursive=False)
    ext = ext_of(p.name)
    if ext in _ZIP_EXTS:
        return ZipComicContainer(env, p)
    if ext in _RAR_EXTS:
        return RarComicContainer(env, p)
    if ext in _SEVEN_EXTS:
        return SevenZipComicContainer(env, p)
    if ext in _TAR_EXTS:
        return TarComicContainer(env, p)
    raise UnsupportedFormatError(f"不支持的漫画容器: {ext or p.name}", reason="未知容器类型")


# ------------------------------------------------------------------ 适配器
class ComicAdapter(FixedAdapter):
    """漫画 / 文件夹图集：图片即页。"""

    def __init__(
        self,
        env: AdapterEnv,
        ref: BookRef,
        *,
        container: Optional[ComicContainer] = None,
        title: Optional[str] = None,
        recursive: bool = False,
    ) -> None:
        super().__init__(env, ref)
        self._container = container or open_container(env, ref.path, recursive=recursive)
        self._title = title or stem_of(ref.name)
        # 真实格式由扩展名 / 容器类型决定（gallery / cbz / cbt …）
        self.format = (ref.format or ext_of(ref.name).lstrip(".") or "gallery").lower()
        self._mime_cache: Dict[str, str] = {}
        self.max_page_bytes = int(env.config.limits.max_page_bytes or 0)
        self.max_source_pixels = int(env.config.limits.max_source_pixels or 0)
        self.jpeg_quality = int(env.config.comic.jpeg_quality)
        self.strip_min_aspect = float(env.config.comic.strip_min_aspect)

    def close(self) -> None:
        self._container.close()

    @property
    def container(self) -> ComicContainer:
        return self._container

    # ------------------------------------------------------------ 基础
    def page_count(self) -> int:
        return len(self._container.names)

    def meta(self) -> AdapterMeta:
        return AdapterMeta(
            title=self._title,
            format=self.format,
            mode="fixed",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=self.page_count(),
            chapters=[],
            supports_json=False,
        )

    def _entry(self, page: int) -> str:
        names = self._container.names
        if page < 1 or page > len(names):
            raise UnsupportedFormatError(f"页码越界: {page}")
        return names[page - 1]

    def raw_bytes(self, page: int) -> Tuple[bytes, str]:
        name = self._entry(page)
        data = self._container.read(name)
        mime = self._mime_cache.get(name) or imaging.sniff_mime(data, ext_of(name))
        self._mime_cache[name] = mime
        return data, mime

    def page_native_size(self, page: int) -> Tuple[int, int]:
        name = self._entry(page)
        data = self._container.read(name)
        size = imaging.probe_size(data)
        if size is None:
            raise ParseError(f"图片无法识别: {name}")
        return size

    def page_aspect(self, page: int) -> float:
        try:
            w, h = self.page_native_size(page)
            return (h / w) if w else 0.0
        except Exception:
            return 0.0

    def page_is_long_strip(self, page: int) -> bool:
        return self.page_aspect(page) >= self.strip_min_aspect

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
        name = self._entry(page)
        data = self._container.read(name)
        mime = self._mime_cache.get(name) or imaging.sniff_mime(data, ext_of(name))
        self._mime_cache[name] = mime

        if fit == "original":
            if not self.max_page_bytes or len(data) <= self.max_page_bytes:
                return data, mime
            # 超订单页字节上限：强制缩放（设计 4.2 / 4.7），但保持原图宽度优先，
            # 否则"原图模式"退化成视口分辨率，用户失去细节
            quality = max(quality, self.jpeg_quality)

        target_w = max(1, int(round(viewport_w * max(0.5, min(2.0, render_scale)))))
        if fit == "original":
            native = imaging.probe_size(data)
            if native and native[0] > target_w:
                target_w = native[0]
        try:
            img = imaging.decode_for_display(data, target_w, self.max_source_pixels)
        except imaging.ImageTooLargeError as exc:
            raise ParseError(f"图片超出像素上限: {name} ({exc})") from exc
        except imaging.ImageDecodeError as exc:
            raise ParseError(f"图片解码失败: {name} ({exc})") from exc

        if img.width > target_w:  # 只缩小，不放大（省流量，客户端 ContentScale.Fit 展示）
            img = imaging.fit_width(img, target_w)
        prefer_jpeg = mime != "image/png" or not _has_alpha(img)
        return imaging.encode_image(
            img, prefer_jpeg=prefer_jpeg, quality=quality or self.jpeg_quality
        )

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        if self.page_count() < 1:
            return None
        target = max(80, min(1024, width or 320))
        try:
            return self.page_bytes(1, fit="viewport", quality=80, viewport_w=target)
        except Exception:
            return None

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
        if n < 1:
            raise ParseError("图集为空")
        first = max(1, min(page, n))
        pages = [first] if first + 1 > n else [first, first + 1]
        imgs = []
        for p in pages:
            data, _ = self.raw_bytes(p)
            target_w = max(1, int(round(viewport_w * render_scale)))
            img = imaging.decode_for_display(data, target_w, self.max_source_pixels)
            if img.width > target_w:
                img = imaging.fit_width(img, target_w)
            imgs.append(img)
        if rtl:
            imgs = list(reversed(imgs))
        merged = imaging.hstack(imgs)
        return imaging.encode_image(merged, prefer_jpeg=True, quality=quality or self.jpeg_quality)

    # ------------------------------------------------------------ 条漫切片
    def strip_pages(self, viewport_h: int, overlap: int = 0) -> List[StripPage]:
        """按视口高度切片；锚点纵向偏移 = 切片在原图中的 y0。"""
        vh = max(64, int(viewport_h))
        # 重叠不能逼近视口高度：否则步进退化成 1px，
        # 一张 3 万像素的长图会切出几万"页"（页表/预取/进度全部爆掉）
        overlap_eff = max(0, min(int(overlap), vh // 2))
        step = max(1, vh - overlap_eff)
        slices: List[StripPage] = []
        for idx, name in enumerate(self._container.names):
            try:
                size = self.page_native_size_for_name(name)
            except Exception:
                size = None
            if size is None:
                # 无法读取尺寸的图片按单页处理，避免整本图集失败
                slices.append(
                    StripPage(
                        image_index=idx,
                        y0=0,
                        height=vh,
                        is_first_of_image=True,
                        is_last_of_image=True,
                    )
                )
                continue
            _, h = size
            y = 0
            while y < h:
                height = min(vh, h - y)
                slices.append(
                    StripPage(
                        image_index=idx,
                        y0=y,
                        height=height,
                        is_first_of_image=(y == 0),
                        is_last_of_image=(y + height >= h),
                    )
                )
                if y + height >= h:
                    break
                y += step
        return slices

    def page_native_size_for_name(self, name: str) -> Optional[Tuple[int, int]]:
        try:
            data = self._container.read(name)
        except Exception:
            return None
        return imaging.probe_size(data)

    def strip_slice_bytes(
        self,
        sp: StripPage,
        *,
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
        fit: str = "viewport",
    ) -> Tuple[bytes, str]:
        names = self._container.names
        if sp.image_index < 0 or sp.image_index >= len(names):
            raise UnsupportedFormatError(f"切片越界: image_index={sp.image_index}")
        name = names[sp.image_index]
        data = self._container.read(name)
        mime = self._mime_cache.get(name) or imaging.sniff_mime(data, ext_of(name))
        self._mime_cache[name] = mime
        try:
            img = imaging.decode_for_display(data, 0, self.max_source_pixels, prefer_jpeg_draft=False)
        except imaging.ImageTooLargeError as exc:
            raise ParseError(f"长图超出像素上限: {name} ({exc})") from exc
        except imaging.ImageDecodeError as exc:
            raise ParseError(f"长图解码失败: {name} ({exc})") from exc
        piece = imaging.crop_slice(img, sp.y0, sp.height)
        # 切片直接裁剪、不缩放（设计 4.7）；PNG/GIF 保持无损，其余按配置质量编码
        if mime in ("image/png", "image/gif"):
            return imaging.encode_png(piece)
        return imaging.encode_image(
            piece, prefer_jpeg=True, quality=quality or self.jpeg_quality
        )


def _has_alpha(img) -> bool:
    try:
        return img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info)
    except Exception:
        return False
