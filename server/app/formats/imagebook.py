"""图片型书籍（漫画）检测与固定版式适配：EPUB 与 MOBI 转换产物共用。

背景（DESIGN.md 13 v0.4.3 / v0.5.1）：KF8 漫画 MOBI 转出的 EPUB、以及原生
"图片即正文"的 EPUB，若走流式排版，每页一张图会被塞进带边距的版心，四周全是
空边、无法铺满屏幕。这里的检测把这类书分流到固定版式漫画管线（每图一页，
fit=viewport），与 CBZ 的阅读体验一致。

检测判据与 ``flow`` 路径构造的块模型完全一致：spine 图片页 ≥8 且平均每页正文
≤64 字符（漫画的文字都在图里）。插图书（大量正文 + 少量插图）不会被误判。

直接 EPUB 的检测在 ``/meta`` 阶段也会执行，因此先用 zip 中央目录的未压缩尺寸
做廉价预筛：正文 XHTML 总量相对图片条目数过大 → 直接判非漫画，不做 HTML
解析。预筛只可能多放过、不会误杀候选：通过预筛后才做与 MOBI 相同的块扫描，
最终判据仍是块模型。
"""

from __future__ import annotations

import threading
from collections import OrderedDict
from pathlib import Path
from typing import Any, Dict, List, Tuple

from ..util.paths import stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    AssetNotFoundError,
    BookRef,
    FixedAdapter,
    ParseError,
)
from .comics import ComicAdapter, ComicContainer
from .epub import EpubAdapter

# 漫画判定阈值：正文极少（平均每页 ≤64 字符，KF8 漫画的文字都在图里）且页数
# 达到"一本书"的量级。插图书（大量正文 + 少量插图）不会被误判。
IMAGE_BOOK_MIN_PAGES = 8
IMAGE_BOOK_MAX_TEXT_PER_PAGE = 64

# 预筛：每个图片条目允许的正文 XHTML 未压缩字节上限。漫画页包裹文件（含 SVG
# 包装）通常 1–3KB，这里取 8KB/图 留足余量（宁可多扫一本插图书，不可漏掉漫画）；
# 正文书动辄数百 KB。预筛只用于决定"要不要做块扫描"，不参与最终判定。
_XHTML_BYTES_PER_IMAGE = 8192

# 检测结果进程内缓存：键 = 文件路径，值 = (mtime, size, info)。
# 检测要解析全部 spine 文档（BeautifulSoup，百页漫画秒级），而适配器是按请求
# 一次性创建的，不缓存等于每次取页都重扫整本。内容修订变化 → mtime/size 变化 →
# 自然失效；上限淘汰最旧条目。
_DETECT_CACHE_LIMIT = 32
_DETECT_CACHE: "OrderedDict[str, Tuple[float, int, Dict[str, Any]]]" = OrderedDict()
_DETECT_LOCK = threading.Lock()


def image_book_info(epub: EpubAdapter) -> Dict[str, Any]:
    """判断 EPUB 是否为图片型漫画，返回 {"manga", "pages", "title", "text_chars"}（带缓存）。"""
    path = Path(epub.ref.path)
    try:
        st = path.stat()
        sig = (st.st_mtime, st.st_size)
    except OSError:
        sig = (0.0, 0)
    key = str(path)
    with _DETECT_LOCK:
        hit = _DETECT_CACHE.get(key)
        if hit is not None and (hit[0], hit[1]) == sig:
            _DETECT_CACHE.move_to_end(key)
            return hit[2]
    info = scan_image_book(epub)
    with _DETECT_LOCK:
        _DETECT_CACHE[key] = (sig[0], sig[1], info)
        while len(_DETECT_CACHE) > _DETECT_CACHE_LIMIT:
            _DETECT_CACHE.popitem(last=False)
    return info


def scan_image_book(epub: EpubAdapter) -> Dict[str, Any]:
    """按 spine 顺序收集图片页并统计正文量（块模型转换与 flow 路径完全一致）。"""
    model = epub.model()
    pages: List[str] = []
    text_chars = 0
    if _prefilter_pass(epub, model):
        for doc in model.docs:
            for block in epub._html_to_blocks(doc.html, doc.name):
                if block.type == "image" and block.asset:
                    pages.append(_container_path(epub, block.asset))
                elif block.is_text:
                    text_chars += len(block.text.strip())
    manga = (
        len(pages) >= IMAGE_BOOK_MIN_PAGES
        and text_chars <= IMAGE_BOOK_MAX_TEXT_PER_PAGE * len(pages)
    )
    return {
        "manga": manga,
        "pages": pages,
        "title": model.title or stem_of(epub.ref.name),
        "text_chars": text_chars,
    }


def _prefilter_pass(epub: EpubAdapter, model) -> bool:
    """zip 元数据廉价预筛：正文 XHTML 总量相对图片数过大 → 不必做块扫描。"""
    image_count = len(model.image_names)
    if image_count < IMAGE_BOOK_MIN_PAGES:
        return False
    xhtml_bytes = sum(epub.entry_size(doc.name) for doc in model.docs)
    return xhtml_bytes <= _XHTML_BYTES_PER_IMAGE * image_count


def _container_path(epub: EpubAdapter, name: str) -> str:
    """把块模型的 asset 路径映射为容器内实际条目名。

    ebooklib 路径下 doc.name 是 manifest href（如 ``text/p1.xhtml``），OPF 位于
    子目录（如 ``OEBPS/``）时图片解析结果会少掉前缀；唯一后缀匹配可补回真实
    条目名，避免漫画取页退化成 ``_read_entry`` 的 basename 兜底（同名多文件时会
    直接报错）。
    """
    if name in epub._zip_names:
        return name
    matches = [e for e in epub._zip_names if e.endswith("/" + name)]
    if len(matches) == 1:
        return matches[0]
    return name


class EpubImageContainer(ComicContainer):
    """把"图片型 EPUB"的 spine 图片暴露成漫画容器（条目名 = EPUB 内路径）。"""

    def __init__(self, env: AdapterEnv, epub: EpubAdapter, names: List[str]) -> None:
        super().__init__(env)
        self._epub = epub
        self._names = list(names)
        # 基类 read_entry/asset（/books/{id}/asset 预览）经 _raw_map → _read_by_real
        # 读取；_finish_init 需要全量条目。顺序保持 spine 顺序（页序由 names 决定，
        # 不做自然排序），尺寸取 EPUB zip 声明值供 _guard_entry 限流。
        self._raw_entries = [
            (n, int(self._epub.entry_size(n) or 0), False) for n in self._names
        ]
        self._finish_init()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        try:
            return self._epub._read_entry(real)
        except AssetNotFoundError as exc:
            raise ParseError(f"EPUB 内不存在页面图片: {real}") from exc

    def read(self, name: str) -> bytes:
        return self._read_by_real(name, name)

    def close(self) -> None:
        return None  # zip 句柄归 EpubImageContainer 所在适配器的 epub 持有


class EpubComicAdapter(FixedAdapter):
    """图片型 EPUB：按固定版式漫画打开，复用漫画管线（每图一页）。

    ``ref`` 是用户可见的书籍（EPUB 本体或 MOBI 原文件），``eref`` 是实际要读取
    的 EPUB（MOBI 时是转换产物）。meta.size 展示 ref 的大小。
    """

    format = "epub"

    def __init__(
        self,
        env: AdapterEnv,
        ref: BookRef,
        eref: BookRef,
        info: Dict[str, Any],
    ) -> None:
        super().__init__(env, ref)
        self._source_size = int(ref.size)
        self._epub = EpubAdapter(env, eref)
        self._comic = ComicAdapter(
            env,
            eref,
            container=EpubImageContainer(env, self._epub, info["pages"]),
            title=str(info["title"]),
        )

    def close(self) -> None:
        self._comic.close()  # 容器 no-op，zip 由 _epub 持有
        self._epub.close()

    def meta(self) -> AdapterMeta:
        meta = self._comic.meta()
        meta.format = self.ref.format or self.format
        meta.size = self._source_size
        return meta

    def page_count(self) -> int:
        return self._comic.page_count()

    def page_bytes(self, page, *, fit="viewport", quality=85, viewport_w=1080, render_scale=1.0):
        return self._comic.page_bytes(
            page, fit=fit, quality=quality, viewport_w=viewport_w, render_scale=render_scale
        )

    def page_native_size(self, page: int) -> Tuple[int, int]:
        return self._comic.page_native_size(page)

    def pair_bytes(self, page, *, quality=85, viewport_w=1080, render_scale=1.0, rtl=False):
        return self._comic.pair_bytes(
            page, quality=quality, viewport_w=viewport_w, render_scale=render_scale, rtl=rtl
        )

    def strip_pages(self, viewport_h: int, overlap: int = 0):
        return self._comic.strip_pages(viewport_h, overlap)

    def strip_slice_bytes(self, sp, *, quality=85, viewport_w=1080, render_scale=1.0, fit="viewport"):
        return self._comic.strip_slice_bytes(
            sp, quality=quality, viewport_w=viewport_w, render_scale=render_scale, fit=fit
        )

    @property
    def supports_strip(self) -> bool:
        return True

    def cover(self, width: int = 0) -> "Tuple[bytes, str] | None":
        return self._comic.cover(width)


def open_epub_adapter(env: AdapterEnv, ref: BookRef) -> "EpubAdapter | EpubComicAdapter":
    """EPUB 工厂：图片型（漫画）分流到固定版式，文本型保持流式（设计 7.3）。"""
    flow = EpubAdapter(env, ref)
    info = image_book_info(flow)
    if not info["manga"]:
        return flow
    flow.close()
    return EpubComicAdapter(env, ref, ref, info)
