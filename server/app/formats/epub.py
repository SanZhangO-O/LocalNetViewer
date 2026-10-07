"""EPUB 适配器（设计 7.3）。

- 主路径：ebooklib 读 spine / toc / DC 元数据；
- 兜底路径（ebooklib 缺失或其 API 变化 / 文件不合规）：zipfile + lxml 手写
  OPF / spine / nav 解析（设计风险 11 的对策）；
- 两条路径共用同一套 HTML→块模型转换与资源读取，保证结果一致；
- meta 阶段只做容器级解析（spine、toc、章标题），首次 layout 才全量解析正文；
- 忽略 CSS（有意为之的统一排版），丢弃 script/style/注释。
"""

from __future__ import annotations

import posixpath
import re
import zipfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple
from urllib.parse import unquote

from ..util import imaging
from ..util.paths import ext_of, stem_of
from ..util.xmlutil import find_local as _find_local, safe_fromstring, text_local as _text_local
from lxml import etree  # noqa: PLC0415
from .base import (
    AdapterEnv,
    AdapterMeta,
    AssetNotFoundError,
    Block,
    BookRef,
    Chapter,
    DocumentModel,
    FlowAdapter,
    ParseError,
)
from .htmlblocks import html_to_blocks as _shared_html_to_blocks, make_soup

_COVER_NAME_RE = re.compile(r"(^|/)cover[^/]*\.(jpe?g|png|gif|webp|bmp)$", re.IGNORECASE)


@dataclass
class _EpubDoc:
    name: str
    title: str
    html: bytes


@dataclass
class _EpubModel:
    title: str = ""
    author: Optional[str] = None
    language: Optional[str] = None
    docs: List[_EpubDoc] = field(default_factory=list)
    image_names: List[str] = field(default_factory=list)
    cover_name: Optional[str] = None
    via: str = "zip"


class EpubAdapter(FlowAdapter):
    format = "epub"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        try:
            self._zip = zipfile.ZipFile(ref.path)
        except (OSError, zipfile.BadZipFile) as exc:
            raise ParseError(f"EPUB 容器无法打开: {exc}") from exc
        self._zip_names = set(self._zip.namelist())
        self._lower_map = {n.lower(): n for n in self._zip_names}
        self._model: Optional[_EpubModel] = None
        self._image_size_cache: Dict[str, Tuple[int, int]] = {}

    def close(self) -> None:
        try:
            self._zip.close()
        except Exception:
            pass

    # ------------------------------------------------------------ 容器读取
    def _read_entry(self, name: str) -> bytes:
        """读取容器内资源，**带单条目大小上限**。

        压缩包守卫（``limits.max_archive_entry_mb``）本来只覆盖漫画容器；
        EPUB 走的是同一个 zipfile，没有守卫就意味着一个声明/解压后 2GB 的
        条目可以直接把内存打爆（zip 炸弹）。这里按流式读取强制上限。
        """
        for cand in (
            name,
            unquote(name),
            name.lstrip("/"),
            posixpath.normpath(name),
        ):
            if cand in self._zip_names:
                return self._read_guarded(cand)
        low = self._lower_map.get(name.lower())
        if low is not None:
            return self._read_guarded(low)
        # basename 兜底只在**唯一匹配**时使用：EPUB 不同子目录下同名资源很常见
        # （text1/img1.jpg 与 text2/img1.jpg），随便取一个会静默串读错误资源。
        base = posixpath.basename(name).lower()
        matches = [e for e in self._zip_names if posixpath.basename(e).lower() == base]
        if len(matches) == 1:
            return self._read_guarded(matches[0])
        if len(matches) > 1:
            raise AssetNotFoundError(
                f"EPUB 内存在多个同名资源，无法消歧: {name}（{len(matches)} 个匹配）"
            )
        raise AssetNotFoundError(f"EPUB 内不存在资源: {name}")

    def entry_size(self, name: str) -> int:
        """条目未压缩字节数（zip 中央目录，不解压）；不存在返回 0。

        图片型检测的廉价预筛用（见 formats/imagebook.py）：判断正文 XHTML 总量
        是否小到值得做块扫描，避免文本 EPUB 在 /meta 阶段被全量解析。
        """
        for cand in (
            name,
            unquote(name),
            name.lstrip("/"),
            posixpath.normpath(name),
        ):
            if cand in self._zip_names:
                return self._zipinfo_size(cand)
        low = self._lower_map.get(name.lower())
        if low is not None:
            return self._zipinfo_size(low)
        # basename 唯一匹配兜底（与 _read_entry 同一消歧规则）
        base = posixpath.basename(name).lower()
        matches = [e for e in self._zip_names if posixpath.basename(e).lower() == base]
        if len(matches) == 1:
            return self._zipinfo_size(matches[0])
        return 0

    def _zipinfo_size(self, entry: str) -> int:
        try:
            return int(self._zip.getinfo(entry).file_size)
        except KeyError:
            return 0

    def _read_guarded(self, entry: str) -> bytes:
        limit = int(self.env.config.limits.max_archive_entry_mb or 0) * 1024 * 1024
        with self._zip.open(entry) as fh:
            if not limit:
                return fh.read()
            data = fh.read(limit + 1)
        if len(data) > limit:
            raise ParseError(
                f"EPUB 条目超过上限（{limit // 1048576}MB）: {entry}"
            )
        return data

    def _resolve(self, base_item: str, href: str) -> str:
        """把相对 href 解析为容器内路径（去掉 fragment，规范化 ../）。"""
        href = unquote(str(href or "").strip())
        if not href or href.startswith("data:"):
            return ""
        href = href.split("#", 1)[0]
        if not href:
            return ""
        if href.startswith("/"):
            return posixpath.normpath(href.lstrip("/"))
        base_dir = posixpath.dirname(base_item)
        return posixpath.normpath(posixpath.join(base_dir, href))

    # ------------------------------------------------------------ 模型加载
    def _check_archive_limits(self) -> None:
        """整包解压上限预检（zip 中央目录的声明值）。

        ebooklib 的 ``read_epub`` 会把 manifest 里每个条目（含图片）整块读入内存；
        自有的 ``_read_guarded`` 只覆盖手写读取路径，管不到它。压缩包守卫必须
        在这里补上，否则一个 30MB 的 EPUB 声明 20GB 条目就能把服务器内存打爆
        （设计 9 / 7.4 的 zip 炸弹防护）。
        """
        limits = self.env.config.limits
        entry_limit = int(limits.max_archive_entry_mb or 0) * 1024 * 1024
        total_limit = int(limits.max_archive_total_mb or 0) * 1024 * 1024
        total = 0
        for info in self._zip.infolist():
            size = int(info.file_size or 0)
            if entry_limit and size > entry_limit:
                raise ParseError(
                    f"EPUB 条目超过上限（{entry_limit // 1048576}MB）: {info.filename}"
                )
            total += size
            if total_limit and total > total_limit:
                raise ParseError(
                    f"EPUB 解压总量超过上限（{total_limit // 1048576}MB）"
                )

    def model(self) -> _EpubModel:
        if self._model is not None:
            return self._model
        self._check_archive_limits()
        loaded = self._load_with_ebooklib()
        if loaded is None or not loaded.docs:
            loaded = self._load_with_zip()
        if loaded is None:
            raise ParseError("EPUB 结构无法解析（缺少 OPF / spine）")
        self._model = loaded
        return loaded

    def _load_with_ebooklib(self) -> Optional[_EpubModel]:
        try:
            import ebooklib  # noqa: PLC0415
            from ebooklib import epub  # noqa: PLC0415
        except Exception:
            return None
        try:
            book = epub.read_epub(str(self.ref.path), options={"ignore_ncx": True})
        except Exception:
            return None

        title = _first_meta(book, "title") or stem_of(self.ref.name)
        author = _first_meta(book, "creator")
        language = _first_meta(book, "language")

        toc_titles: Dict[str, str] = {}
        try:
            _flatten_toc(book.toc, toc_titles)
        except Exception:
            pass

        docs: List[_EpubDoc] = []
        try:
            spine = list(book.spine)
        except Exception:
            spine = []
        for entry in spine:
            idref = entry[0] if isinstance(entry, (tuple, list)) and entry else entry
            item = None
            try:
                item = book.get_item_with_id(str(idref))
            except Exception:
                item = None
            if item is None:
                continue
            media = str(getattr(item, "media_type", "") or "")
            if not media.startswith("application/xhtml"):
                continue
            try:
                name = str(item.get_name())
                html = bytes(item.get_content())
            except Exception:
                continue
            norm = posixpath.normpath(name)
            docs.append(_EpubDoc(name=norm, title=toc_titles.get(norm, ""), html=html))

        image_names = [
            n for n in self._zip_names if ext_of(n) in (".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp")
        ]
        model = _EpubModel(
            title=title,
            author=author,
            language=language,
            docs=docs,
            image_names=sorted(image_names),
            cover_name=_guess_cover(image_names),
            via=f"ebooklib/{getattr(ebooklib, 'VERSION', '?')}",
        )
        return model

    def _load_with_zip(self) -> Optional[_EpubModel]:
        try:
            container_xml = self._read_entry("META-INF/container.xml")
        except AssetNotFoundError:
            return None
        try:
            container = safe_fromstring(container_xml)
        except etree.XMLSyntaxError:
            return None
        rootfiles = _find_local(container, "rootfile")
        if not rootfiles:
            return None
        opf_path = posixpath.normpath(str(rootfiles[0].get("full-path") or ""))
        if not opf_path:
            return None
        try:
            opf = safe_fromstring(self._read_entry(opf_path))
        except (AssetNotFoundError, etree.XMLSyntaxError):
            return None

        title = _text_local(opf, "title") or stem_of(self.ref.name)
        author = _text_local(opf, "creator")
        language = _text_local(opf, "language")

        manifest: Dict[str, Dict[str, str]] = {}
        for item in _find_local(opf, "item"):
            iid = str(item.get("id") or "")
            if not iid:
                continue
            manifest[iid] = {
                "href": str(item.get("href") or ""),
                "media_type": str(item.get("media-type") or ""),
                "properties": str(item.get("properties") or ""),
            }

        docs: List[_EpubDoc] = []
        for itemref in _find_local(opf, "itemref"):
            iid = str(itemref.get("idref") or "")
            info = manifest.get(iid)
            if not info:
                continue
            if not str(info["media_type"]).startswith("application/xhtml"):
                continue
            name = posixpath.normpath(
                posixpath.join(posixpath.dirname(opf_path), unquote(info["href"]))
            )
            try:
                html = self._read_entry(name)
            except AssetNotFoundError:
                continue
            docs.append(_EpubDoc(name=name, title="", html=html))

        # 目录：EPUB3 nav 优先，其次 NCX
        toc_titles: Dict[str, str] = {}
        nav_item = next(
            (i for i in manifest.values() if "nav" in str(i.get("properties", "")).split()),
            None,
        )
        if nav_item:
            nav_name = posixpath.normpath(
                posixpath.join(posixpath.dirname(opf_path), unquote(nav_item["href"]))
            )
            try:
                toc_titles.update(_toc_from_nav(self._read_entry(nav_name), nav_name))
            except Exception:
                pass
        if not toc_titles:
            for info in manifest.values():
                if info["media_type"] == "application/x-dtbncx+xml":
                    ncx_name = posixpath.normpath(
                        posixpath.join(posixpath.dirname(opf_path), unquote(info["href"]))
                    )
                    try:
                        toc_titles.update(_toc_from_ncx(self._read_entry(ncx_name), ncx_name))
                    except Exception:
                        pass
                    break

        for doc in docs:
            doc.title = toc_titles.get(doc.name, "")

        image_names = [
            n for n in self._zip_names if ext_of(n) in (".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp")
        ]
        return _EpubModel(
            title=title,
            author=author,
            language=language,
            docs=docs,
            image_names=sorted(image_names),
            cover_name=_guess_cover(image_names),
            via="zip+opf",
        )

    # ------------------------------------------------------------ meta
    def meta(self) -> AdapterMeta:
        model = self.model()
        chapters = [
            {"index": i, "title": doc.title or stem_of(doc.name), "total_chars": None}
            for i, doc in enumerate(model.docs)
        ]
        return AdapterMeta(
            title=model.title or stem_of(self.ref.name),
            format=self.format,
            mode="flow",
            author=model.author,
            language=model.language,
            size=self.ref.size,
            page_count=None,
            chapters=chapters,
            supports_json=True,
        )

    # ------------------------------------------------------------ 解析
    def document(self) -> DocumentModel:
        model = self.model()
        chapters: List[Chapter] = []
        total_chars = 0
        for i, doc in enumerate(model.docs):
            blocks = self._html_to_blocks(doc.html, doc.name)
            title = doc.title
            if not title:
                for b in blocks:
                    if b.type == "heading" and b.text.strip():
                        title = b.text.strip()
                        break
            if not title:
                title = stem_of(doc.name)
            chapter = Chapter(index=i, title=title, blocks=blocks)
            total_chars += chapter.total_chars
            chapters.append(chapter)
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": model.title or stem_of(self.ref.name),
                "author": model.author,
                "language": model.language,
                "total_chars": total_chars,
                "source": self.ref.vpath,
                "parser": model.via,
            },
            chapters=chapters,
        )

    # ------------------------------------------------------------ HTML → 块
    def _html_to_blocks(self, html: bytes, item_name: str) -> List[Block]:
        """委托公共转换器（formats/htmlblocks.py）；图片解析回落到容器条目。"""

        def _resolve(tag, src: str) -> Optional[Block]:
            s = str(src).strip()
            if not s or s.startswith("data:"):
                # 内联 data: URI 无法通过 /asset 提供（asset 不接受空路径），
                # 与其留一个永久损坏的图片块，不如丢弃（封面另有专门路径）
                return None
            resolved = self._resolve(item_name, s)
            if not resolved:
                return None
            w, h = self._image_size(resolved)
            return Block.image(resolved, w, h)

        return _shared_html_to_blocks(html, _resolve)

    def _image_size(self, name: str) -> Tuple[int, int]:
        if name in self._image_size_cache:
            return self._image_size_cache[name]
        size = (600, 800)
        try:
            data = self._read_entry(name)
            probed = imaging.probe_size(data)
            if probed:
                size = probed
        except Exception:
            pass
        self._image_size_cache[name] = size
        return size

    # ------------------------------------------------------------ 封面/资源
    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        try:
            model = self.model()
        except ParseError:
            model = None
        name = model.cover_name if model else None
        if name is None and model and model.image_names:
            name = model.image_names[0]
        if name:
            try:
                data = self._read_entry(name)
            except AssetNotFoundError:
                data = None
            if data:
                if not width:
                    return data, imaging.sniff_mime(data, ext_of(name))
                try:
                    img = imaging.decode_for_display(data, width, self.env.config.limits.max_source_pixels)
                    if img.width > width:
                        img = imaging.fit_width(img, width)
                    return imaging.encode_image(img, prefer_jpeg=True, quality=84)
                except Exception:
                    return data, imaging.sniff_mime(data, ext_of(name))
        from ..render.raster import text_cover

        title = model.title if model else stem_of(self.ref.name)
        author = model.author if model else None
        return text_cover(title, author, width=width or 320, fonts=self.env.fonts)

    def asset(self, path: str) -> Tuple[bytes, str]:
        norm = posixpath.normpath(str(path).replace("\\", "/").lstrip("/"))
        data = self._read_entry(norm)
        return data, imaging.sniff_mime(data, ext_of(norm))


# ------------------------------------------------------------------ 辅助
def _first_meta(book, key: str) -> Optional[str]:
    try:
        values = book.get_metadata("DC", key)
    except Exception:
        return None
    for entry in values or []:
        value = entry[0] if isinstance(entry, (tuple, list)) and entry else entry
        if value:
            return str(value).strip() or None
    return None


def _flatten_toc(items, out: Dict[str, str], depth: int = 0) -> None:
    if depth > 8 or not items:
        return
    for item in items:
        if isinstance(item, (tuple, list)) and len(item) == 2:
            section, children = item
            href = getattr(section, "href", None)
            title = getattr(section, "title", None)
            if href and title:
                out.setdefault(_norm_href(str(href)), str(title))
            _flatten_toc(children, out, depth + 1)
            continue
        href = getattr(item, "href", None)
        title = getattr(item, "title", None)
        if href and title:
            out.setdefault(_norm_href(str(href)), str(title))


def _norm_href(href: str) -> str:
    h = unquote(href).split("#", 1)[0]
    return posixpath.normpath(h.lstrip("/"))


def _toc_from_nav(html: bytes, nav_name: str) -> Dict[str, str]:
    soup = make_soup(html)
    if soup is None:
        return {}
    out: Dict[str, str] = {}
    for tag in soup.find_all("a"):
        href = tag.get("href")
        text = tag.get_text(" ", strip=True)
        if not href or not text:
            continue
        target = posixpath.normpath(
            posixpath.join(posixpath.dirname(nav_name), unquote(str(href)).split("#", 1)[0])
        )
        out.setdefault(target, text)
    return out


def _toc_from_ncx(xml: bytes, ncx_name: str) -> Dict[str, str]:
    try:
        root = safe_fromstring(xml)
    except etree.XMLSyntaxError:
        return {}
    out: Dict[str, str] = {}
    for nav_point in _find_local(root, "navPoint"):
        label = _text_local(nav_point, "text")
        src = None
        for el in _find_local(nav_point, "content"):
            src = el.get("src")
            if src:
                break
        if not src:
            continue
        target = posixpath.normpath(
            posixpath.join(posixpath.dirname(ncx_name), unquote(str(src)).split("#", 1)[0])
        )
        if label:
            out.setdefault(target, label)
    return out


def _guess_cover(image_names: Sequence[str]) -> Optional[str]:
    if not image_names:
        return None
    for name in image_names:
        if _COVER_NAME_RE.search(name):
            return name
    for name in image_names:
        if "cover" in posixpath.basename(name).lower():
            return name
    return None
