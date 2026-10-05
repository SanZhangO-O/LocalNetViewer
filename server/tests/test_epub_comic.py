"""图片型 EPUB 回归（DESIGN.md 13 v0.4.4）：图片为主的 EPUB 按固定版式漫画打开。

背景：EPUB 此前一律走流式排版，每页一张图的漫画被塞进带边距的版心，图片缩小、
四周全是空边，无法铺满屏幕。现在与 MOBI 共用 ``formats/imagebook.py`` 的检测
（spine 图片页 ≥8 且平均每页正文 ≤64 字符）→ 分流到 ``EpubComicAdapter``，
完整复用漫画管线（fit=viewport/original、双页跨页、条漫、封面）。

文本型 EPUB 行为不变（流式排版）；检测的廉价预筛（zip 未压缩尺寸）保证
文本 EPUB 的 /meta 不会被全量解析拖慢。
"""

from __future__ import annotations

import io
from pathlib import Path

from PIL import Image

from app.formats import registry
from app.formats.base import BookRef, FixedAdapter
from app.formats.epub import EpubAdapter
from app.formats.imagebook import EpubComicAdapter, image_book_info, scan_image_book
from tests.test_mobi_comic import _make_epub

BOOK_ID = "epubmanga0000001"


def _ref(path: Path, name: str = "manga.epub") -> BookRef:
    st = path.stat()
    return BookRef(
        book_id=BOOK_ID,
        vpath=f"/lib/{name}",
        path=path,
        format="epub",
        size=st.st_size,
        mtime=st.st_mtime,
    )


# ------------------------------------------------------------------ 检测
def test_scan_detects_image_epub_and_keeps_spine_order(env, tmp_path: Path):
    path = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    with EpubAdapter(env, _ref(path)) as adapter:
        info = scan_image_book(adapter)
    assert info["manga"] is True
    assert len(info["pages"]) == 10
    assert info["pages"][0].endswith("p001.jpeg")
    assert info["pages"][-1].endswith("p010.jpeg")
    assert info["title"] == "测试漫画名"


def test_scan_rejects_text_heavy_epub(env, tmp_path: Path):
    path = _make_epub(tmp_path / "novel.epub", pages=8, text_per_page=400)
    with EpubAdapter(env, _ref(path)) as adapter:
        info = scan_image_book(adapter)
    assert info["manga"] is False


def test_scan_rejects_tiny_image_epub(env, tmp_path: Path):
    path = _make_epub(tmp_path / "tiny.epub", pages=3, text_per_page=0)
    with EpubAdapter(env, _ref(path)) as adapter:
        info = scan_image_book(adapter)
    assert info["manga"] is False


def test_prefilter_skips_block_scan_for_large_text(env, tmp_path: Path):
    # 正文 XHTML 未压缩总量远超 4KB/图 → 廉价预筛直接拒绝，不做 HTML 块扫描
    path = _make_epub(tmp_path / "big.epub", pages=8, text_per_page=4000)
    with EpubAdapter(env, _ref(path)) as adapter:
        info = scan_image_book(adapter)
    assert info["manga"] is False
    assert info["pages"] == []


def test_detect_cache_invalidates_on_change(env, tmp_path: Path):
    path = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    with EpubAdapter(env, _ref(path)) as adapter:
        assert image_book_info(adapter)["manga"] is True
    # 同一路径内容变化（mtime/size 变化）→ 缓存失效并重新检测
    _make_epub(path, pages=10, text_per_page=4000)
    with EpubAdapter(env, _ref(path)) as adapter:
        assert image_book_info(adapter)["manga"] is False


# ------------------------------------------------------------------ 工厂分流
def test_factory_routes_image_epub_to_comic_adapter(env, tmp_path: Path):
    path = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    ref = _ref(path)
    adapter = registry.create_adapter(env, ref)
    try:
        assert isinstance(adapter, EpubComicAdapter)
        assert isinstance(adapter, FixedAdapter)
        meta = adapter.meta()
        assert meta.mode == "fixed"
        assert meta.format == "epub"
        assert meta.page_count == 10
        assert meta.title == "测试漫画名"
        assert meta.size == ref.size  # 展示原始 EPUB 大小

        assert adapter.supports_strip is True
        assert adapter.page_native_size(1) == (200, 300)
        assert adapter.page_native_size(10) == (290, 390)  # 页序 = spine 顺序

        body, mime = adapter.page_bytes(1, fit="viewport", viewport_w=400)
        assert mime.startswith("image/") and body
        assert Image.open(io.BytesIO(body)).size == (200, 300)

        cover = adapter.cover(120)
        assert cover is not None and cover[0] and cover[1].startswith("image/")
    finally:
        adapter.close()


def test_factory_keeps_text_epub_flow(env, tmp_path: Path):
    path = _make_epub(tmp_path / "novel.epub", pages=8, text_per_page=400, title="文字书")
    adapter = registry.create_adapter(env, _ref(path, "novel.epub"))
    try:
        assert isinstance(adapter, EpubAdapter)
        assert not isinstance(adapter, FixedAdapter)
        meta = adapter.meta()
        assert meta.mode == "flow"
        assert meta.format == "epub"
    finally:
        adapter.close()
