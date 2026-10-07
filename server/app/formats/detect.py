"""内容嗅探：扩展名缺失/错误时的格式兜底（M5 Phase 0）。

只做**廉价**判定：读文件头（zip 走中央目录条目名，不解压内容），把"未识别
扩展名"的文件归入可打开的格式之一；判定不出返回 ``None``，保持"普通文件"
分类。所有探测都有预算：gzip 只读首个 tar 成员头，zip 的 ``mimetype`` 只读
前 64 字节，绝不整包解压。

误判防线：
- 文本启发式要求可打印率足够高且样本可解码，二进制文件不会当文本；
- zip 先看前 64 字节的 ``mimetype`` 与条目名判断 epub/odt/docx/xlsx/pptx；
- Office 判定与实际解析必要条件同源（如 docx 要求 word/document.xml），
  避免"列出可打开、点开 422"；
- gz 只有能真正按 tar 打开时才判 tar（单文件 .gz 不硬塞）。
"""

from __future__ import annotations

import tarfile
import zipfile
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Optional

from ..util import imaging
from ..util.textutil import decode_text

MAX_ENTRIES_FOR_KIND = 20000


@dataclass(frozen=True)
class Detected:
    """嗅探结果：kind ∈ text | image | book | none；format 为注册表格式 id。"""

    kind: str
    format: str = ""


NONE = Detected("none")


def detect_file(path: Path | str, head_bytes: int = 16384) -> Detected:
    """带缓存的文件嗅探：键为 (路径, 大小, mtime_ns)，内容不变不重复读盘。"""
    p = Path(path)
    try:
        st = p.stat()
    except OSError:
        return NONE
    return _detect_file_cached(str(p), int(st.st_size), int(st.st_mtime_ns), int(head_bytes))


@lru_cache(maxsize=4096)
def _detect_file_cached(path: str, size: int, mtime_ns: int, head_bytes: int) -> Detected:
    try:
        with open(path, "rb") as fh:
            head = fh.read(max(4096, head_bytes))
    except OSError:
        return NONE
    if not head:
        return NONE
    return detect_bytes(head, path=Path(path))


def detect_bytes(head: bytes, *, path: Optional[Path] = None) -> Detected:
    # ------------------------------------------------------------ 魔数
    if head.startswith(b"%PDF"):
        return Detected("book", "pdf")
    if head[:4] == b"PK\x03\x04":
        return _detect_zip(path)
    if head[:4] == b"Rar!":
        return Detected("book", "rar")
    if head[:6] == b"7z\xbc\xaf\x27\x1c":
        return Detected("book", "7z")
    if head[:2] == b"\x1f\x8b":
        return _detect_gz(path)
    if len(head) > 262 and head[257:262] == b"ustar":
        return Detected("book", "tar")

    # ------------------------------------------------------------ 图片
    if imaging.sniff_mime(head, "").startswith("image/"):
        return Detected("image")
    if _looks_like_exotic_image(head):
        return Detected("image")

    # ------------------------------------------------------------ 文本
    if _looks_like_text(head):
        return Detected("text")
    return NONE


def _looks_like_exotic_image(head: bytes) -> bool:
    if len(head) >= 12 and head[4:8] == b"ftyp" and head[8:12] in (
        b"heic", b"heix", b"hevc", b"hevx", b"mif1", b"msf1", b"avif", b"avis",
    ):
        return True
    if head[:4] == b"8BPS":
        return True
    if head[:2] == b"\xff\x0a" or head[:12].endswith(b"JXL \r\n\x87\n"):
        return True
    return False


def _looks_like_text(head: bytes) -> bool:
    """可解码 + 可打印率足够高 → 当纯文本。"""
    if b"\x00" in head[:4096] and not (head[:2] in (b"\xff\xfe", b"\xfe\xff")):
        # NUL 通常是二进制；UTF-16 BOM 排除在外（decode_text 会正确处理）
        return False
    text, _enc = decode_text(head)
    if not text:
        return False
    sample = text[:8192]
    bad = sum(1 for ch in sample if ord(ch) < 32 and ch not in "\t\r\n\f\v")
    return (bad / max(1, len(sample))) < 0.02


def _read_zip_entry_head(zf: zipfile.ZipFile, name: str, limit: int = 64) -> bytes:
    """只读条目开头 ``limit`` 字节（判定用；不整条解压）。"""
    try:
        with zf.open(name) as fh:
            return fh.read(limit)
    except Exception:
        return b""


def _detect_zip(path: Optional[Path]) -> Detected:
    if path is None:
        return Detected("book", "zip")
    try:
        with zipfile.ZipFile(path) as zf:
            names = zf.namelist()
            if len(names) > MAX_ENTRIES_FOR_KIND:
                return Detected("book", "zip")
            lowered = {n.lower() for n in names}
            # ODF/EPUB 规范要求首条 mimetype；只读前 64 字节即可区分
            if "mimetype" in lowered:
                mime = _read_zip_entry_head(zf, _match_case(names, "mimetype"))
                if mime.startswith(b"application/epub+zip"):
                    return Detected("book", "epub")
                if mime.startswith(b"application/vnd.oasis.opendocument.text"):
                    return Detected("book", "odt")
                if mime.startswith(b"application/vnd.oasis.opendocument"):
                    # ods/odp 等暂不支持：宁可当作未知，也不误判成 ODT 文本
                    return NONE
            if "meta-inf/container.xml" in lowered and any(
                n.lower().endswith(".opf") for n in names
            ):
                return Detected("book", "epub")
            if "[content_types].xml" in lowered:
                # 与实际解析必要条件同源（officebook 要求这些文件存在）
                if "word/document.xml" in lowered:
                    return Detected("book", "docx")
                if "xl/workbook.xml" in lowered:
                    return Detected("book", "xlsx")
                if any(n.lower().startswith("ppt/slides/slide") for n in names):
                    return Detected("book", "pptx")
            if "content.xml" in lowered and "meta.xml" in lowered:
                return Detected("book", "odt")
    except Exception:
        return Detected("book", "zip")
    return Detected("book", "zip")


def _match_case(names, target: str) -> str:
    for name in names:
        if name.lower() == target:
            return name
    return target


def _detect_gz(path: Optional[Path]) -> Detected:
    if path is None:
        return NONE
    try:
        with tarfile.open(str(path), "r:*") as tf:
            # 只读第一个成员头（getmembers 会为压缩流解压整包，列表路径上不可接受）
            if tf.next() is None:
                return NONE
        return Detected("book", "tar")
    except Exception:
        return NONE
