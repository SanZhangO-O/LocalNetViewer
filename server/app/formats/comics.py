"""漫画容器与文件夹图集适配器（设计 3.6 / 7.4 / 7.5）。

容器统一抽象：CBZ/ZIP、CBR/RAR、CB7/7z、CBT/TAR 与"图片文件夹即书"。

要点：
- 只收录图片扩展名白名单，按自然排序（数字感知，忽略大小写）；
- 跳过 ``__MACOSX/``、隐藏条目、``Thumbs.db`` 等（可配置）；
- ZIP 按条目随机读取（不解压整包）；RAR/7z 需系统解压后端，缺失时给出明确原因；
- 条目名编码兜底：非 UTF-8 条目按 cp437 解码后启发式尝试 GBK / Shift-JIS；
- 安全：条目名规范化（防 zip-slip）、单条目与总解压大小上限（防 zip 炸弹）。
- 条漫 strip：按视口高度切片，锚点 ``(0, 源图索引, 纵向偏移)``。
- 浏览模式（设计 7.6）：压缩包容器（browsable=True）额外提供全量条目枚举
  （``browse()``，含非图片条目、目录合成）与任意条目读取（``read_entry()``），
  供"压缩包内容浏览"使用；安全规则与图片分页完全一致。
"""

from __future__ import annotations

import logging
import math
import os
import posixpath
import tarfile
import threading
import time
import zipfile
from abc import ABC, abstractmethod
from collections import OrderedDict
from dataclasses import dataclass
from fnmatch import fnmatch
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from ..util import imaging
from ..util.natural import natural_key
from ..util.paths import ext_key_of, ext_of, stem_of
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

_ZIP_EXTS = {".cbz", ".zip", ".cbz3", ".fbz"}
_RAR_EXTS = {".cbr", ".rar"}
_SEVEN_EXTS = {".cb7", ".7z"}
_TAR_EXTS = {".cbt", ".tar", ".tgz", ".tar.gz", ".tbz2", ".tar.bz2", ".txz", ".tar.xz"}
# 外部 libarchive（bsdtar）容器：CAB / LZH / LHA / ACE（ACE 实际不被 libarchive 支持，会明确失败）
_EXTERNAL_EXTS = {".cab", ".lzh", ".lha", ".ace"}
_ISO_EXTS = {".iso"}
# 可浏览压缩包（设计 7.6）：contents API 只对这些容器开放
ARCHIVE_EXTS = _ZIP_EXTS | _RAR_EXTS | _SEVEN_EXTS | _TAR_EXTS | _EXTERNAL_EXTS | _ISO_EXTS

logger = logging.getLogger("localnetviewer.formats.comics")


def is_archive_name(name: str) -> bool:
    return ext_key_of(name) in ARCHIVE_EXTS


# ------------------------------------------------------------------ 工具
def is_image_name(name: str) -> bool:
    """图片**页**条目判定：单图可解码扩展名 + TIFF（容器/文件夹内的页，M5）。

    独立 ``.tif/.tiff`` 文件在书库列表按多页格式（``tiff``)打开，不经过这里；
    这里的白名单只服务"页"（压缩包条目、文件夹图集）。
    """
    return ext_key_of(name) in imaging.page_image_exts()


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


# ------------------------------------------------------------------ 目录图片枚举缓存
# 图集（文件夹虚拟书）可能含上万张图片。枚举本身不贵，但 /meta、/layout 与**每个**
# 取页请求都会重新打开适配器：若不缓存，每次都重扫整棵目录并逐文件 stat，代价随
# 图片数线性放大，翻页与预取会明显卡顿。这里按 (目录, recursive, skip 规则) 缓存
# 自然排序后的相对名列表：
# - 只凭 DirEntry 判定类型，不对每个文件单独 stat；
# - 目录 mtime 未变且距上次扫描 < 2s 时复用；mtime 一变（增删/改名）立即重算；
# - recursive 模式下目录 mtime 不反映子目录增删，由 TTL 兜底（与 gallery rev 记忆
#   同一取舍，见 gallery.GALLERY_MEMO_TTL_S）。
FOLDER_LISTING_TTL_S = 2.0
_FOLDER_LISTING_CACHE_LIMIT = 64
_FOLDER_LISTING_LOCK = threading.Lock()
_FOLDER_LISTING: "OrderedDict[tuple, tuple]" = OrderedDict()


def _scan_folder_images(root: Path, recursive: bool, patterns: Sequence[str]) -> List[str]:
    """枚举目录下的图片（相对路径，自然排序）；跳过隐藏、符号链接与 skip 规则。

    用 ``os.scandir`` 递归而不是 ``os.walk``：``DirEntry`` 的类型判定走目录项
    缓存，避免对每个文件多做一次 ``os.path.islink`` / ``is_file`` 的 stat ——
    上万张图时这是每请求的主要开销。
    """
    out: List[str] = []
    base = Path(root)
    if recursive:
        stack = [base]
        while stack:
            current = stack.pop()
            try:
                entries = list(os.scandir(current))
            except OSError:
                continue
            for entry in entries:
                try:
                    if entry.is_symlink():
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        if not entry.name.startswith("."):
                            stack.append(Path(entry.path))
                        continue
                    if not entry.is_file(follow_symlinks=False):
                        continue
                except OSError:
                    continue
                rel = os.path.relpath(entry.path, base).replace("\\", "/")
                if should_skip_entry(rel, patterns) or not is_image_name(rel):
                    continue
                out.append(rel)
    else:
        try:
            entries = list(os.scandir(base))
        except OSError:
            return out
        for entry in entries:
            try:
                if entry.is_symlink() or not entry.is_file(follow_symlinks=False):
                    continue
            except OSError:
                continue
            if should_skip_entry(entry.name, patterns) or not is_image_name(entry.name):
                continue
            out.append(entry.name)
    return _sort_names(out)


def list_folder_images(
    root: Path, recursive: bool = False, patterns: Sequence[str] = ()
) -> List[str]:
    """带缓存的目录图片枚举（返回副本，调用方可安全持有/修改）。"""
    base = Path(root)
    pats = tuple(patterns or ())
    key = (str(base), bool(recursive), pats)
    try:
        mtime = float(os.stat(base).st_mtime)
    except OSError:
        mtime = 0.0
    now = time.monotonic()
    with _FOLDER_LISTING_LOCK:
        hit = _FOLDER_LISTING.get(key)
        if (
            hit is not None
            and abs(float(hit[0]) - mtime) < 1e-6
            and (now - float(hit[1])) < FOLDER_LISTING_TTL_S
        ):
            _FOLDER_LISTING.move_to_end(key)
            return list(hit[2])
    names = _scan_folder_images(base, bool(recursive), pats)
    with _FOLDER_LISTING_LOCK:
        _FOLDER_LISTING[key] = (mtime, now, names)
        _FOLDER_LISTING.move_to_end(key)
        while len(_FOLDER_LISTING) > _FOLDER_LISTING_CACHE_LIMIT:
            _FOLDER_LISTING.popitem(last=False)
    return list(names)


# ------------------------------------------------------------------ 容器
@dataclass(frozen=True)
class ArchiveEntry:
    """压缩包内条目（浏览模式，设计 7.6）：规范化后的相对路径。"""

    name: str
    size: int = 0
    is_dir: bool = False


def synthesize_dirs(
    raw_entries: Sequence[Tuple[str, int, bool]], patterns: Sequence[str]
) -> List[ArchiveEntry]:
    """原始条目 (名, 字节, 是否目录) → 规范化 + 跳过规则 + 目录合成 + 自然排序。

    浏览模式与图片分页共用同一套安全规则（``should_skip_entry`` /
    ``is_unsafe_entry_name``）：隐藏条目、``__MACOSX``、zip-slip 名一律不可见。
    压缩包不强制含目录条目（``a/b.png`` 可以没有 ``a/`` 记录），父目录按文件
    条目合成。
    """
    dirs: Dict[str, int] = {}
    files: Dict[str, int] = {}
    seen: set = set()
    for name, size, is_dir in raw_entries:
        raw = str(name).replace("\\", "/")
        if raw in seen:
            continue
        seen.add(raw)
        norm = posixpath.normpath(raw)
        if is_unsafe_entry_name(norm) or should_skip_entry(norm, patterns):
            continue
        if is_dir:
            dirs[norm] = 0
        else:
            files[norm] = int(size or 0)
        parts = norm.split("/")[:-1] if not is_dir else norm.split("/")
        for i in range(1, len(parts) + 1):
            dirs.setdefault("/".join(parts[:i]), 0)
    out = [
        ArchiveEntry(name=n, size=s, is_dir=False)
        for n, s in files.items()
        if n not in dirs
    ]
    out.extend(
        ArchiveEntry(name=n, size=0, is_dir=True) for n in dirs if n not in files
    )
    out.sort(key=lambda e: natural_key(e.name))
    return out


class ComicContainer(ABC):
    """漫画容器：只暴露"图片条目名列表 + 按名读取"。"""

    # 浏览模式（设计 7.6）只有真正的压缩包容器提供；文件夹/单图容器为 False
    browsable: bool = False

    def __init__(self, env: AdapterEnv) -> None:
        self.env = env
        self.patterns = list(env.config.comic.skip_entries)
        self.max_entry_bytes = int(env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024
        self.max_total_bytes = int(env.config.limits.max_archive_total_mb or 0) * 1024 * 1024
        # 子类构造时填充：全量原始条目（已解码名），供浏览模式使用
        self._raw_entries: List[Tuple[str, int, bool]] = []
        # 规范化名 → 容器内真实名 / 声明尺寸（**全部文件条目**，不做图片过滤）
        self._raw_map: Dict[str, str] = {}
        self._raw_sizes: Dict[str, int] = {}

    @property
    @abstractmethod
    def names(self) -> List[str]:
        """图片条目名（已自然排序）。"""

    @abstractmethod
    def _read_by_real(self, real: str, key: str) -> bytes:
        """按容器内真实名读取条目字节（流式限流由各实现负责）。"""

    def _finish_init(self) -> None:
        """构造收尾：从 _raw_entries 构建图片分页映射（原有语义不变）。"""
        mapping: Dict[str, str] = {}
        sizes: Dict[str, int] = {}
        total = 0
        for name, size, is_dir in self._raw_entries:
            if is_dir:
                continue
            if should_skip_entry(name, self.patterns) or not is_image_name(name):
                continue
            norm = posixpath.normpath(name)
            if is_unsafe_entry_name(norm):
                continue
            if norm in mapping:
                # 规范化后同名（a/b.png 与 a/./b.png）：保留自然排序较小者并告警，
                # 静默覆盖会让页数与实际条目数对不上
                keep, drop = sorted([mapping[norm], name])
                logger.warning(
                    "容器内条目规范化后重名: %s 与 %s（保留 %s）",
                    drop,
                    name,
                    keep,
                )
                if keep != name:
                    continue
            mapping[norm] = name
            sizes[norm] = int(size or 0)
            total += sizes[norm]
        self._guard_total(total)
        self._map = mapping
        self._sizes = sizes
        self._names = _sort_names(list(mapping.keys()))
        # 浏览映射：全部文件条目（跳过规则在 browse() 里统一应用，这里不做过滤，
        # 以便 read_entry 可以读取被分页白名单排除的非图片条目）
        raw_map: Dict[str, str] = {}
        raw_sizes: Dict[str, int] = {}
        for name, size, is_dir in self._raw_entries:
            if is_dir:
                continue
            norm = posixpath.normpath(name)
            if is_unsafe_entry_name(norm) or norm in raw_map:
                continue
            raw_map[norm] = name
            raw_sizes[norm] = int(size or 0)
        self._raw_map = raw_map
        self._raw_sizes = raw_sizes

    def browse(self) -> List[ArchiveEntry]:
        """全量条目（含非图片），目录合成，自然排序（设计 7.6）。"""
        return synthesize_dirs(self._raw_entries, self.patterns)

    def read_entry(self, name: str) -> bytes:
        """按规范化名读取**任意**文件条目（浏览模式；不受图片白名单限制）。"""
        key = posixpath.normpath(name)
        real = self._raw_map.get(key)
        if real is None:
            raise AssetNotFoundError(f"压缩包内不存在条目: {name}")
        self._guard_entry(key, self._raw_sizes.get(key, 0))
        return self._read_by_real(real, key)

    def entry_size(self, name: str) -> int:
        return self._raw_sizes.get(posixpath.normpath(name), 0)

    def read(self, name: str) -> bytes:
        """读取图片页条目（分页白名单内的名字）。"""
        key = posixpath.normpath(name)
        real = self._map.get(key)
        if real is None:
            raise AssetNotFoundError(f"压缩包内不存在条目: {name}")
        self._guard_entry(key, self.size_of(key))
        return self._read_by_real(real, key)

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

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
    browsable = True

    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        # zipfile.ZipFile 的共享句柄并非线程安全（读取会移动共享文件对象）。
        # 当前调用模式是"单适配器单线程"所以安全，但这只是约定——一旦适配器层
        # 做预取/并发解码就会交错读写。与 RAR / 7z 的锁保持一致，成本极低。
        self._io_lock = threading.Lock()
        try:
            self._zip = zipfile.ZipFile(path)
        except (OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"ZIP 容器无法打开: {exc}") from exc
        raw: List[Tuple[str, int, bool]] = []
        for info in self._zip.infolist():
            name = decode_zip_name(info).replace("\\", "/")
            raw.append((name, int(info.file_size or 0), bool(info.is_dir())))
        self._raw_entries = raw
        self._finish_init()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        try:
            with self._io_lock:
                with self._zip.open(real) as fh:
                    return self._read_stream(fh, key)
        except ParseError:
            raise
        except (KeyError, OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"条目读取失败 {key}: {exc}") from exc

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
    browsable = True

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
        raw: List[Tuple[str, int, bool]] = []
        for info in self._rf.infolist():
            is_dir = bool(getattr(info, "is_dir", lambda: False)())
            raw.append(
                (
                    str(info.filename).replace("\\", "/"),
                    int(getattr(info, "file_size", 0) or 0),
                    is_dir,
                )
            )
        self._raw_entries = raw
        self._finish_init()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        try:
            with self._rf.open(real) as fh:
                # 与 ZIP/TAR 一致地流式限流：只信包头声明的尺寸会被 zip 炸弹绕过
                return self._read_stream(fh, key)
        except ParseError:
            raise
        except Exception as exc:
            raise ParseError(f"RAR 条目读取失败 {key}: {exc}") from exc

    def close(self) -> None:
        try:
            self._rf.close()
        except Exception:
            pass


class SevenZipComicContainer(ComicContainer):
    browsable = True

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
        raw: List[Tuple[str, int, bool]] = []
        for info in infos:
            raw.append(
                (
                    str(info.filename).replace("\\", "/"),
                    int(getattr(info, "uncompressed", 0) or 0),
                    bool(getattr(info, "is_directory", False)),
                )
            )
        self._raw_entries = raw
        self._finish_init()
        self._lock = threading.Lock()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        with self._lock:
            try:
                self._arc.reset()
            except Exception:
                pass
            # py7zr 1.x 删除了 read()/readall()：内存提取改走
            # extract(targets, factory=BytesIOFactory(limit))；0.x 仍走 read()。
            if hasattr(type(self._arc), "read"):
                try:
                    result = self._arc.read(targets=[real])
                except Exception as exc:
                    raise ParseError(f"7z 条目读取失败 {key}: {exc}") from exc
                data = result.get(real) if isinstance(result, dict) else None
                if data is None:
                    for k, v in (result or {}).items():
                        if posixpath.normpath(str(k)) == key:
                            data = v
                            break
                if data is None:
                    raise AssetNotFoundError(f"7z 内不存在条目: {key}")
                payload = data.read() if hasattr(data, "read") else bytes(data)
            else:
                from py7zr.io import BytesIOFactory  # noqa: PLC0415

                limit = self.max_entry_bytes or (512 * 1024 * 1024)
                factory = BytesIOFactory(limit=limit)
                try:
                    self._arc.extract(targets=[real], factory=factory)
                except Exception as exc:
                    raise ParseError(f"7z 条目读取失败 {key}: {exc}") from exc
                try:
                    data = factory.get(real)
                except KeyError:
                    data = None
                if data is None:
                    for k, v in factory.products.items():
                        if posixpath.normpath(str(k)) == key:
                            data = v
                            break
                if data is None:
                    raise AssetNotFoundError(f"7z 内不存在条目: {key}")
                payload = data.read() if hasattr(data, "read") else bytes(data)
        # 7z 无法按条目流式限流，只能读后校验（声明尺寸已在 _guard_entry 校验过一次）
        return self._check_size(key, payload)

    def close(self) -> None:
        try:
            self._arc.close()
        except Exception:
            pass


class TarComicContainer(ComicContainer):
    browsable = True

    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            self._tar = tarfile.open(str(path), "r:*")
        except Exception as exc:
            raise ParseError(f"TAR 容器无法打开: {exc}") from exc
        raw: List[Tuple[str, int, bool]] = []
        for member in self._tar.getmembers():
            raw.append(
                (
                    str(member.name).replace("\\", "/"),
                    int(member.size or 0),
                    not member.isfile(),
                )
            )
        self._raw_entries = raw
        self._finish_init()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        try:
            fh = self._tar.extractfile(real)
        except KeyError:
            fh = None
        if fh is None:
            raise AssetNotFoundError(f"TAR 内不存在条目: {key}")
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

    def _safe_size(self, name: str) -> int:
        """扫描与 stat 之间文件可能被删除/改名，这里不能让 OSError 冒出来。"""
        try:
            return int(os.path.getsize(self.root / name))
        except OSError:
            return 0

    def _scan(self) -> List[str]:
        if self._single is not None:
            if should_skip_entry(self._single, self.patterns):
                return []
            if is_image_name(self._single):
                return [self._single]
            # 错扩展名/无扩展名的单图（嗅探注册的图集）：按魔数兜底
            if imaging.looks_like_image_file(self.root / self._single):
                return [self._single]
            return []
        # 共享缓存：resolve() 的图片计数与这里取页共用同一份枚举结果，避免同一
        # 目录被反复重扫（图集可能有上万张图）。
        return list_folder_images(self.root, self.recursive, self.patterns)

    @property
    def names(self) -> List[str]:
        return self._names

    def size_of(self, name: str) -> int:
        # 目录容器不在构造时对每张图 stat（上万张图会让每次打开都卡在文件系统上）。
        # 只有显式索取某条目的尺寸时才 stat 一次。
        return self._safe_size(posixpath.normpath(name))

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

    def _read_by_real(self, real: str, key: str) -> bytes:
        # 文件夹容器没有"真实名"映射（read 自己做穿越校验），直接复用 read
        return self.read(key)


def open_container(env: AdapterEnv, path: Path, *, recursive: bool = False) -> ComicContainer:
    """按扩展名选择容器；目录 / 单张图片（含嗅探）→ 图集容器。"""
    p = Path(path)
    if p.is_dir():
        return FolderComicContainer(env, p, recursive=recursive)
    ext = ext_key_of(p.name)
    if ext in _ZIP_EXTS:
        return ZipComicContainer(env, p)
    if ext in _RAR_EXTS:
        return RarComicContainer(env, p)
    if ext in _SEVEN_EXTS:
        return SevenZipComicContainer(env, p)
    if ext in _TAR_EXTS:
        return TarComicContainer(env, p)
    if ext in _EXTERNAL_EXTS:
        from .external import BsdtarComicContainer  # noqa: PLC0415 - 延迟导入避免环

        return BsdtarComicContainer(env, p)
    if ext in _ISO_EXTS:
        from .isocomic import IsoComicContainer  # noqa: PLC0415 - 延迟导入避免环

        return IsoComicContainer(env, p)
    magic = _open_by_magic(env, p)
    if magic is not None:
        # 错扩展名 / 无扩展名的压缩包：按魔数兜底（嗅探注册的 zip 等）
        return magic
    if is_image_name(p.name) or imaging.looks_like_image_file(p):
        # 单张图片 = 单页图集（设计 7.5）；错扩展名/无扩展名由魔数兜底
        return FolderComicContainer(env, p, recursive=False)
    raise UnsupportedFormatError(f"不支持的漫画容器: {ext or p.name}", reason="未知容器类型")


def _open_by_magic(env: AdapterEnv, path: Path) -> Optional[ComicContainer]:
    try:
        with open(path, "rb") as fh:
            head = fh.read(512)
    except OSError:
        return None
    if head[:4] == b"PK\x03\x04":
        return ZipComicContainer(env, path)
    if head[:4] == b"Rar!":
        return RarComicContainer(env, path)
    if head[:6] == b"7z\xbc\xaf\x27\x1c":
        return SevenZipComicContainer(env, path)
    if head[:2] == b"\x1f\x8b" or (len(head) > 262 and head[257:262] == b"ustar"):
        return TarComicContainer(env, path)
    return None


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
    def supports_strip(self) -> bool:
        """漫画 / 图集容器才支持条漫切片（设计 7.4）。"""
        return True

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

    # ------------------------------------------------------------ 浏览模式（设计 7.6）
    def archive_info(self) -> Optional[Dict[str, int]]:
        """压缩包内容概览：条目数 / 图片数；非压缩包容器（文件夹/单图）返回 None。"""
        if not getattr(self._container, "browsable", False):
            return None
        entries = self._container.browse()
        files = [e for e in entries if not e.is_dir]
        return {
            "entry_count": len(files),
            "image_count": sum(1 for e in files if is_image_name(e.name)),
        }

    def asset(self, path: str) -> Tuple[bytes, str]:
        """压缩包内任意条目的原始字节（图片预览用；上限校验在 read_entry 内）。"""
        data = self._container.read_entry(path)
        return data, imaging.sniff_mime(data, ext_of(path))

    def page_native_size(self, page: int) -> Tuple[int, int]:
        name = self._entry(page)
        data = self._container.read(name)
        size = imaging.probe_size(data, ext_of(name))
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
            native = imaging.probe_size(data, ext_of(name))
            if native and native[0] > target_w:
                target_w = native[0]
        try:
            img = imaging.decode_for_display(
                data, target_w, self.max_source_pixels, ext=ext_of(name)
            )
        except imaging.ImageTooLargeError as exc:
            raise ParseError(f"图片超出像素上限: {name} ({exc})") from exc
        except imaging.ImageDecodeError as exc:
            raise ParseError(f"图片解码失败: {name} ({exc})") from exc

        if img.width > target_w:  # 只缩小，不放大（省流量，客户端 ContentScale.Fit 展示）
            img = imaging.fit_width(img, target_w)
        prefer_jpeg = mime != "image/png" or not _has_alpha(img)
        q = quality or self.jpeg_quality
        encoded = imaging.encode_image(img, prefer_jpeg=prefer_jpeg, quality=q)
        if fit == "original" and self.max_page_bytes and len(encoded[0]) > self.max_page_bytes:
            # 原图超单页字节上限：提高质量参数不够（PNG/照片重编码后仍可能超），
            # 必须真正按字节预算缩小（设计 4.2"超出强制缩放"）。
            encoded = _shrink_to_byte_cap(img, prefer_jpeg, q, self.max_page_bytes)
        return encoded

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
            img = imaging.decode_for_display(
                data, target_w, self.max_source_pixels, ext=ext_of(self._entry(p))
            )
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
        return imaging.probe_size(data, ext_of(name))

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
            img = imaging.decode_for_display(
                data, 0, self.max_source_pixels, prefer_jpeg_draft=False, ext=ext_of(name)
            )
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


def _shrink_to_byte_cap(
    img, prefer_jpeg: bool, quality: int, cap: int, attempts: int = 4
) -> Tuple[bytes, str]:
    """把已编码仍超字节上限的图片逐步缩小重编码，直到落入上限（尽力而为）。

    单次缩放比例按 ``sqrt(cap / size)`` 估算（字节数≈像素数×常数），
    每次至少缩 1 像素，避免估算偏差导致原地打转；到上限仍打不住就返回
    最后一版（至少比原图小），不会无限重试。
    """
    data, mime = imaging.encode_image(img, prefer_jpeg=prefer_jpeg, quality=quality)
    for _ in range(max(0, attempts)):
        if len(data) <= cap:
            break
        ratio = min(0.95, math.sqrt(float(cap) / float(len(data))) * 0.95)
        new_w = max(1, int(img.width * ratio))
        if new_w >= img.width:
            new_w = max(1, img.width - 1)
        img = imaging.fit_width(img, new_w)
        data, mime = imaging.encode_image(img, prefer_jpeg=prefer_jpeg, quality=quality)
    return data, mime
