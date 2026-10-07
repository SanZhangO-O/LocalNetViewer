"""Office 文档适配器（M5 Phase 2）：DOCX / ODT / PPTX / XLSX + 旧版格式转换。

设计取舍：
- 全部走**内置** zip + lxml 解析，不引入硬依赖；解析目标是"能读"而不是
  像素级还原（与 EPUB 忽略 CSS 的取舍一致）；
- 旧版 .doc/.xls/.ppt/.wps/.pps 经 LibreOffice headless 转成对应 OOXML/ODF
  后复用同一解析链（探测缺失 → 列表置灰）；
- 章节：DOCX/ODT/Markdown 式的标题层级 → 章；PPTX 一张幻灯片一章；
  XLSX 一个工作表一章（每行一段）。
"""

from __future__ import annotations

import os
import posixpath
import re
import shutil
import subprocess
import zipfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from lxml import etree

from ..util.concurrency import KeyedLocks
from ..util.paths import makedirs_quiet, stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    Block,
    BookRef,
    Chapter,
    DocumentModel,
    FlowAdapter,
    ParseError,
    UnsupportedFormatError,
)
from .htmlbook import split_chapters_by_headings

# ------------------------------------------------------------------ 通用工具


def _local(tag: object) -> str:
    if not isinstance(tag, str):
        return ""
    return tag.rsplit("}", 1)[-1]


def _iter_local(root, name: str):
    for el in root.iter():
        if _local(el.tag) == name:
            yield el


def _first_local(root, name: str):
    for el in _iter_local(root, name):
        return el
    return None


def _text_of(el) -> str:
    return "".join(el.itertext())


def _attr_local(el, name: str) -> Optional[str]:
    for key, value in el.attrib.items():
        if _local(key) == name:
            return str(value)
    return None


def _read_zip_entry(zf: zipfile.ZipFile, name: str, limit: int = 0) -> Optional[bytes]:
    """按名读取容器条目；``limit > 0`` 时先查声明尺寸、再流式限流（防 zip 炸弹）。"""
    low = name.lower()
    candidates = [name, name.lstrip("/")]
    for entry in zf.namelist():
        if entry.lower() == low:
            candidates.append(entry)
    for cand in candidates:
        try:
            info = zf.getinfo(cand)
        except KeyError:
            continue
        if limit and int(info.file_size) > limit:
            raise ParseError(f"文档条目超过上限（{limit // 1048576}MB）: {name}")
        try:
            with zf.open(cand) as fh:
                if limit:
                    data = fh.read(limit + 1)
                    if len(data) > limit:
                        raise ParseError(
                            f"文档条目超过上限（{limit // 1048576}MB）: {name}"
                        )
                    return data
                return fh.read()
        except KeyError:
            continue
    return None


def _open_zip(path: Path) -> zipfile.ZipFile:
    try:
        return zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as exc:
        raise ParseError(f"文档容器无法打开: {exc}") from exc


def _parse_xml(data: bytes):
    # 禁外部实体/网络/超大树：文档 XML 都来自本地文件，但不给 XXE 留口子
    parser = etree.XMLParser(resolve_entities=False, no_network=True, huge_tree=False)
    try:
        return etree.fromstring(data, parser)
    except etree.XMLSyntaxError as exc:
        raise ParseError(f"XML 解析失败: {exc}") from exc


class _ZipXmlAdapter(FlowAdapter):
    """zip+XML 类 Office 文档的公共骨架。"""

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self._blocks_cache: Optional[List[Block]] = None
        self._chapters_cache: Optional[List[Chapter]] = None
        self._title_cache: Optional[str] = None
        # 单条目解压上限（与压缩包容器同配置）：防高压缩比 Office zip 炸弹
        self._entry_limit = int(env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024

    def _check_limit(self) -> None:
        max_mb = int(self.env.config.limits.max_book_mb or 0)
        if max_mb and (self.ref.size or 0) > max_mb * 1024 * 1024:
            raise ParseError(f"文件超过上限 {max_mb}MB")

    def _doc_title(self) -> str:
        return stem_of(self.ref.name)

    def _title(self) -> str:
        if self._title_cache is None:
            self._title_cache = self._doc_title() or stem_of(self.ref.name)
        return self._title_cache

    def _parse_blocks(self) -> List[Block]:
        raise NotImplementedError

    def _blocks(self) -> List[Block]:
        if self._blocks_cache is None:
            self._check_limit()
            self._blocks_cache = self._parse_blocks()
        return self._blocks_cache

    def _chapters(self) -> List[Chapter]:
        if self._chapters_cache is None:
            self._chapters_cache = split_chapters_by_headings(self._blocks(), self._title())
        return self._chapters_cache

    def meta(self) -> AdapterMeta:
        chapters = self._chapters()
        return AdapterMeta(
            title=self._title(),
            format=self.format,
            mode="flow",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=None,
            chapters=[
                {"index": c.index, "title": c.title, "total_chars": c.total_chars}
                for c in chapters
            ],
            supports_json=True,
        )

    def document(self) -> DocumentModel:
        chapters = self._chapters()
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": self._title(),
                "author": None,
                "language": None,
                "total_chars": sum(c.total_chars for c in chapters),
                "source": self.ref.vpath,
            },
            chapters=chapters,
        )

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        from ..render.raster import text_cover

        return text_cover(self._title(), None, width=width or 320, fonts=self.env.fonts)


# ------------------------------------------------------------------ DOCX
_HEADING_STYLE_RE = re.compile(r"^(?:heading|标题)\s*(\d+)", re.IGNORECASE)


class DocxAdapter(_ZipXmlAdapter):
    format = "docx"

    def _doc_title(self) -> str:
        with _open_zip(Path(self.ref.path)) as zf:
            core = _read_zip_entry(zf, "docProps/core.xml", self._entry_limit)
        if core:
            try:
                root = _parse_xml(core)
                title = _first_local(root, "title")
                if title is not None and _text_of(title).strip():
                    return _text_of(title).strip()
            except ParseError:
                pass
        return stem_of(self.ref.name)

    def _parse_blocks(self) -> List[Block]:
        with _open_zip(Path(self.ref.path)) as zf:
            doc = _read_zip_entry(zf, "word/document.xml", self._entry_limit)
        if doc is None:
            raise ParseError("DOCX 缺少 word/document.xml")
        root = _parse_xml(doc)
        body = _first_local(root, "body")
        if body is None:
            return []
        blocks: List[Block] = []
        self._walk_docx(body, blocks)
        return blocks

    def _walk_docx(self, el, blocks: List[Block]) -> None:
        for child in el:
            name = _local(child.tag)
            if name == "p":
                block = self._paragraph_block(child)
                if block is not None:
                    blocks.append(block)
            elif name == "tbl":
                self._table_blocks(child, blocks)
            elif name in ("sdt", "sdtContent", "body", "tc"):
                self._walk_docx(child, blocks)

    def _paragraph_block(self, p) -> Optional[Block]:
        text_parts: List[str] = []
        for node in p.iter():
            tag = _local(node.tag)
            if tag == "t":
                text_parts.append(node.text or "")
            elif tag in ("br", "cr"):
                text_parts.append(" ")
        text = "".join(text_parts).strip()
        style = None
        ppr = _first_local(p, "pPr")
        if ppr is not None:
            pstyle = _first_local(ppr, "pStyle")
            if pstyle is not None:
                style = _attr_local(pstyle, "val")
        is_list = ppr is not None and _first_local(ppr, "numPr") is not None
        if not text:
            return None
        if style:
            m = _HEADING_STYLE_RE.match(style) or re.match(r"^(\d+)$", style)
            if m:
                return Block.heading(text, level=int(m.group(1)))
        if is_list:
            return Block.paragraph("• " + text)
        return Block.paragraph(text)

    def _table_blocks(self, tbl, blocks: List[Block]) -> None:
        for row in _iter_local(tbl, "tr"):
            cells: List[str] = []
            for cell in row:
                if _local(cell.tag) != "tc":
                    continue
                texts: List[str] = []
                for p in _iter_local(cell, "p"):
                    t = "".join(node.text or "" for node in p.iter() if _local(node.tag) == "t").strip()
                    if t:
                        texts.append(t)
                cells.append(" ".join(texts))
            line = " | ".join(c for c in cells if c)
            if line.strip():
                blocks.append(Block.paragraph(line))


# ------------------------------------------------------------------ ODT
class OdtAdapter(_ZipXmlAdapter):
    format = "odt"

    def _doc_title(self) -> str:
        try:
            with _open_zip(Path(self.ref.path)) as zf:
                meta = _read_zip_entry(zf, "meta.xml", self._entry_limit)
        except ParseError:
            meta = None
        if meta:
            try:
                root = _parse_xml(meta)
                title = _first_local(root, "title")
                if title is not None and _text_of(title).strip():
                    return _text_of(title).strip()
            except ParseError:
                pass
        return stem_of(self.ref.name)

    def _parse_blocks(self) -> List[Block]:
        with _open_zip(Path(self.ref.path)) as zf:
            content = _read_zip_entry(zf, "content.xml", self._entry_limit)
        if content is None:
            raise ParseError("ODT 缺少 content.xml")
        root = _parse_xml(content)
        body = _first_local(root, "body")
        if body is None:
            return []
        blocks: List[Block] = []
        self._walk_odt(body, blocks)
        return blocks

    def _walk_odt(self, el, blocks: List[Block]) -> None:
        for child in el:
            name = _local(child.tag)
            if name == "h":
                text = _text_of(child).strip()
                if text:
                    try:
                        level = int(_attr_local(child, "outline-level") or "1")
                    except ValueError:
                        level = 1
                    blocks.append(Block.heading(text, level=max(1, min(6, level))))
            elif name == "p":
                text = _text_of(child).strip()
                if text:
                    blocks.append(Block.paragraph(" ".join(text.split())))
            elif name == "table":
                self._table_blocks(child, blocks)
            elif name in (
                "text", "body", "list", "list-item", "section", "note", "frame", "text-box",
                "table-row", "table-cell",
            ):
                self._walk_odt(child, blocks)

    def _table_blocks(self, table, blocks: List[Block]) -> None:
        for row in _iter_local(table, "table-row"):
            cells: List[str] = []
            for cell in row:
                if _local(cell.tag) != "table-cell":
                    continue
                text = " ".join(_text_of(cell).split())
                cells.append(text)
            line = " | ".join(c for c in cells if c)
            if line.strip():
                blocks.append(Block.paragraph(line))


# ------------------------------------------------------------------ PPTX
class PptxAdapter(_ZipXmlAdapter):
    format = "pptx"

    def _slide_names(self) -> List[str]:
        with _open_zip(Path(self.ref.path)) as zf:
            names = [n for n in zf.namelist() if re.match(r"ppt/slides/slide\d+\.xml$", n, re.IGNORECASE)]

        def order(name: str) -> int:
            m = re.search(r"(\d+)", posixpath.basename(name))
            return int(m.group(1)) if m else 0

        return sorted(names, key=order)

    def _parse_blocks(self) -> List[Block]:
        names = self._slide_names()
        if not names:
            raise ParseError("PPTX 没有幻灯片")
        blocks: List[Block] = []
        with _open_zip(Path(self.ref.path)) as zf:
            for idx, name in enumerate(names):
                data = _read_zip_entry(zf, name, self._entry_limit)
                if data is None:
                    continue
                root = _parse_xml(data)
                paragraphs: List[str] = []
                for p in _iter_local(root, "p"):
                    texts = [node.text or "" for node in p.iter() if _local(node.tag) == "t"]
                    line = "".join(texts).strip()
                    if line:
                        paragraphs.append(line)
                if not paragraphs:
                    continue
                blocks.append(Block.heading(f"幻灯片 {idx + 1}", 1))
                for line in paragraphs:
                    blocks.append(Block.paragraph(line))
        return blocks


# ------------------------------------------------------------------ XLSX
_COL_RE = re.compile(r"([A-Z]+)")


def _col_index(ref: str) -> int:
    m = _COL_RE.match(ref.upper())
    if not m:
        return 0
    idx = 0
    for ch in m.group(1):
        idx = idx * 26 + (ord(ch) - ord("A") + 1)
    return max(0, idx - 1)


class XlsxAdapter(_ZipXmlAdapter):
    format = "xlsx"
    MAX_ROWS_PER_SHEET = 20000
    MAX_CELL_CHARS = 200

    def _sheets(self) -> List[Tuple[str, str]]:
        """[(sheet 名, zip 内 xml 路径)]，按工作簿顺序。"""
        with _open_zip(Path(self.ref.path)) as zf:
            wb = _read_zip_entry(zf, "xl/workbook.xml", self._entry_limit)
            rels = _read_zip_entry(zf, "xl/_rels/workbook.xml.rels", self._entry_limit)
        if wb is None:
            raise ParseError("XLSX 缺少 xl/workbook.xml")
        rel_map: Dict[str, str] = {}
        if rels:
            rel_root = _parse_xml(rels)
            for rel in _iter_local(rel_root, "Relationship"):
                rid = _attr_local(rel, "Id")
                target = _attr_local(rel, "Target")
                if rid and target:
                    target = target.replace("\\", "/").lstrip("/")
                    if not target.startswith("xl/"):
                        target = posixpath.normpath(posixpath.join("xl", target))
                    rel_map[rid] = target
        root = _parse_xml(wb)
        out: List[Tuple[str, str]] = []
        for sheet in _iter_local(root, "sheet"):
            name = _attr_local(sheet, "name") or "Sheet"
            rid = _attr_local(sheet, "id") or ""
            target = rel_map.get(rid, "")
            if target:
                out.append((name, target))
        return out

    def _parse_blocks(self) -> List[Block]:
        sheets = self._sheets()
        if not sheets:
            raise ParseError("XLSX 没有工作表")
        with _open_zip(Path(self.ref.path)) as zf:
            shared = self._shared_strings(zf)
            blocks: List[Block] = []
            for name, path in sheets:
                data = _read_zip_entry(zf, path, self._entry_limit)
                if data is None:
                    continue
                blocks.append(Block.heading(name, 1))
                blocks.extend(self._sheet_blocks(data, shared))
        return blocks

    def _shared_strings(self, zf: zipfile.ZipFile) -> List[str]:
        data = _read_zip_entry(zf, "xl/sharedStrings.xml", self._entry_limit)
        if not data:
            return []
        out: List[str] = []
        try:
            root = _parse_xml(data)
        except ParseError:
            return []
        for si in _iter_local(root, "si"):
            out.append("".join(si.itertext()).strip())
        return out

    def _sheet_blocks(self, data: bytes, shared: List[str]) -> List[Block]:
        try:
            root = _parse_xml(data)
        except ParseError:
            return []
        blocks: List[Block] = []
        rows = 0
        for row in _iter_local(root, "row"):
            rows += 1
            if rows > self.MAX_ROWS_PER_SHEET:
                blocks.append(Block.paragraph("…（表格过大，仅显示前 %d 行）" % self.MAX_ROWS_PER_SHEET))
                break
            cells: List[Tuple[int, str]] = []
            for c in row:
                if _local(c.tag) != "c":
                    continue
                ref = _attr_local(c, "r") or ""
                ctype = _attr_local(c, "t")
                value = self._cell_value(c, ctype, shared)
                if value:
                    cells.append((_col_index(ref), value))
            if not cells:
                continue
            cells.sort(key=lambda item: item[0])
            line = " | ".join(v for _, v in cells)
            if line.strip():
                blocks.append(Block.paragraph(line))
        return blocks

    def _cell_value(self, c, ctype: Optional[str], shared: List[str]) -> str:
        if ctype == "s":
            v = _first_local(c, "v")
            if v is not None and v.text:
                try:
                    return shared[int(v.text)][: self.MAX_CELL_CHARS]
                except (ValueError, IndexError):
                    return ""
            return ""
        if ctype == "inlineStr":
            text = "".join(c.itertext()).strip()
            return text[: self.MAX_CELL_CHARS]
        v = _first_local(c, "v")
        if v is not None and v.text is not None:
            return str(v.text)[: self.MAX_CELL_CHARS]
        text = "".join(c.itertext()).strip()
        return text[: self.MAX_CELL_CHARS]


# ------------------------------------------------------------------ 旧版格式转换
_LEGACY_TARGET = {
    "office_legacy_doc": "odt",
    "office_legacy_xls": "xlsx",
    "office_legacy_ppt": "pptx",
}

_LEGACY_CLASS = {
    "odt": OdtAdapter,
    "xlsx": XlsxAdapter,
    "pptx": PptxAdapter,
}

_CONVERT_LOCKS = KeyedLocks()


def soffice_path() -> Optional[str]:
    """LibreOffice 可执行文件路径（探测结果供 registry 复用，单一来源）。"""
    return shutil.which("soffice") or shutil.which("soffice.exe")


def _convert_with_soffice(env: AdapterEnv, ref: BookRef, exe: str, target_ext: str) -> Path:
    out_dir = env.config.cache_dir / "converted" / "lo"
    makedirs_quiet(out_dir)
    rev = str(getattr(ref, "revision", "") or "")
    target = out_dir / (f"{ref.book_id}.{rev}.{target_ext}" if rev else f"{ref.book_id}.{target_ext}")
    with _CONVERT_LOCKS.acquire(str(target)):
        if target.is_file() and target.stat().st_size > 0:
            return target
        tmp_dir = out_dir / f"{ref.book_id}.{rev or '0'}.tmp.{os.getpid()}"
        makedirs_quiet(tmp_dir)
        timeout = max(60.0, float(env.config.limits.parse_timeout_s) * 4)
        # 每次转换独立 profile：避免共享 profile 的并发冲突，也不需要全局串行锁
        profile = tmp_dir / "lo-profile"
        makedirs_quiet(profile)
        try:
            proc = subprocess.run(
                [
                    exe,
                    f"-env:UserInstallation={profile.as_uri()}",
                    "--headless",
                    "--norestore",
                    "--convert-to",
                    target_ext,
                    "--outdir",
                    str(tmp_dir),
                    str(ref.path),
                ],
                capture_output=True,
                timeout=timeout,
            )
            produced = [p for p in tmp_dir.glob(f"*.{target_ext}") if p.is_file()]
            if proc.returncode != 0 or not produced:
                raise ParseError(
                    f"LibreOffice 转换失败（exit={proc.returncode}），详情见服务器日志"
                )
            os.replace(produced[0], target)
        except subprocess.TimeoutExpired as exc:
            raise ParseError(f"LibreOffice 转换超时（>{timeout:.0f}s）") from exc
        except OSError as exc:
            raise ParseError(f"LibreOffice 转换无法执行: {exc}") from exc
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)
    return target


def open_legacy_adapter(env: AdapterEnv, ref: BookRef, kind: str):
    """旧版 Office 格式：LibreOffice 转换后复用对应解析器。"""
    exe = soffice_path()
    if not exe:
        raise UnsupportedFormatError(
            "旧版 Office 格式需要 LibreOffice 转换，请安装后重试",
            reason="需安装 LibreOffice",
        )
    target_ext = _LEGACY_TARGET.get(kind, "odt")
    converted = _convert_with_soffice(env, ref, exe, target_ext)
    try:
        size = int(converted.stat().st_size)
        mtime = float(converted.stat().st_mtime)
    except OSError:
        size, mtime = 0, 0.0
    synth = BookRef(
        book_id=ref.book_id,
        vpath=ref.vpath,
        path=converted,
        format=target_ext,
        size=size,
        mtime=mtime,
        virtual=ref.virtual,
        revision=ref.revision,
        extra=dict(ref.extra or {}),
    )
    cls = _LEGACY_CLASS.get(target_ext, OdtAdapter)
    return cls(env, synth)
