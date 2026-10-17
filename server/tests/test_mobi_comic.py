"""漫画型 MOBI 回归（DESIGN.md 13 v0.4.3）：图片型 EPUB 必须按固定版式漫画打开。

背景：KF8 漫画 MOBI 经 Calibre 转出的 EPUB 是"每页一张图"，此前仍走流式
排版，图片被塞进带边距的版心 —— 漫画四周全是空边、无法适应屏幕。修复后由
`open_mobi_adapter` 检测并分流到 `MobiComicAdapter`（漫画管线，fit=viewport）。

不依赖 Calibre：工厂测试用假的 ``subprocess.run`` 写出预制 EPUB 充当转换产物，
同时钉住"转换输出路径以 .epub 结尾"的契约。
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path
from types import SimpleNamespace

from PIL import Image

from app.formats import mobi as mobi_mod
from app.formats.base import BookRef, FixedAdapter
from app.formats.epub import EpubAdapter
from app.formats.imagebook import scan_image_book
from tests.conftest import jpeg_bytes

BOOK_ID = "mobimanga0000001"


# ------------------------------------------------------------------ 夹具
def _page_image(i: int) -> bytes:
    # 每页尺寸不同：用 native_size 钉住"页序 = spine 顺序"
    return jpeg_bytes(200 + i * 10, 300 + i * 10)


def _make_epub(path: Path, *, pages: int, text_per_page: int, title: str = "测试漫画名") -> Path:
    """造一个最小 EPUB：每章一图 + 指定字数的正文（text_per_page=0 即纯图片）。"""
    manifest: list[str] = []
    spine: list[str] = []
    extra: list[tuple[str, bytes]] = []
    for i in range(pages):
        iid = f"p{i + 1:03d}"
        name = f"text/{iid}.xhtml"
        text = "字" * text_per_page
        html = (
            '<?xml version="1.0" encoding="utf-8"?>'
            '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>'
            f"<body><p>{text}</p><p><img src=\"images/{iid}.jpeg\"/></p></body></html>"
        )
        manifest.append(
            f'<item id="{iid}" href="{name}" media-type="application/xhtml+xml"/>'
        )
        spine.append(f'<itemref idref="{iid}"/>')
        extra.append((f"OEBPS/{name}", html.encode("utf-8")))
        extra.append((f"OEBPS/images/{iid}.jpeg", _page_image(i)))
    opf = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="i">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:identifier id="i">urn:uuid:test</dc:identifier>'
        f"<dc:title>{title}</dc:title><dc:language>zh</dc:language>"
        "</metadata>"
        "<manifest>" + "".join(manifest) + "</manifest>"
        "<spine>" + "".join(spine) + "</spine>"
        "</package>"
    )
    container = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf"'
        ' media-type="application/oebps-package+xml"/></rootfiles></container>'
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", container)
        zf.writestr("OEBPS/content.opf", opf)
        for name, data in extra:
            zf.writestr(name, data)
    return path


def _scan(env, path: Path) -> dict:
    ref = BookRef(book_id=BOOK_ID, vpath="/lib/m.epub", path=path, format="epub")
    adapter = EpubAdapter(env, ref)
    try:
        return scan_image_book(adapter)
    finally:
        adapter.close()


def _mobi_ref(tmp_path: Path, name: str) -> BookRef:
    src = tmp_path / name
    src.write_bytes(b"fake mobi")
    return BookRef(
        book_id=BOOK_ID,
        vpath=f"/lib/{name}",
        path=src,
        format="mobi",
        size=src.stat().st_size,
        mtime=src.stat().st_mtime,
    )


# ------------------------------------------------------------------ 检测
def test_scan_detects_manga_and_keeps_spine_order(env, tmp_path: Path):
    path = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    info = _scan(env, path)
    assert info["manga"] is True
    assert len(info["pages"]) == 10
    assert info["pages"][0].endswith("p001.jpeg")
    assert info["pages"][-1].endswith("p010.jpeg")
    assert info["title"] == "测试漫画名"


def test_scan_rejects_text_heavy_book(env, tmp_path: Path):
    path = _make_epub(tmp_path / "novel.epub", pages=8, text_per_page=400)
    info = _scan(env, path)
    assert info["manga"] is False
    assert len(info["pages"]) == 8


def test_scan_rejects_tiny_image_book(env, tmp_path: Path):
    # 少量图片的"纯图片 EPUB"（如只有封面的残书）不按漫画打开
    path = _make_epub(tmp_path / "tiny.epub", pages=3, text_per_page=0)
    info = _scan(env, path)
    assert info["manga"] is False


# ------------------------------------------------------------------ 漫画适配器
def test_mobi_comic_adapter_serves_fixed_pages(env, tmp_path: Path):
    path = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    info = _scan(env, path)
    stat = path.stat()
    eref = BookRef(
        book_id=BOOK_ID, vpath="/lib/仁医01.mobi", path=path,
        format="epub", size=stat.st_size, mtime=stat.st_mtime,
    )
    ref = BookRef(
        book_id=BOOK_ID, vpath="/lib/仁医01.mobi", path=path,
        format="mobi", size=4048, mtime=1.0,
    )
    adapter = mobi_mod.MobiComicAdapter(env, ref, eref, info)
    try:
        assert adapter.mode == "fixed"
        meta = adapter.meta()
        assert meta.mode == "fixed" and meta.format == "mobi"
        assert meta.page_count == 10
        assert meta.title == "测试漫画名"
        assert meta.size == 4048  # 展示原始 MOBI 大小，不是转换产物大小
        assert adapter.supports_strip is True

        assert adapter.page_count() == 10
        assert adapter.page_native_size(1) == (200, 300)
        assert adapter.page_native_size(10) == (290, 390)  # 页序 = spine 顺序

        body, mime = adapter.page_bytes(1, fit="viewport", viewport_w=400)
        assert mime.startswith("image/") and body
        assert Image.open(io.BytesIO(body)).size == (200, 300)

        pair, pmime = adapter.pair_bytes(1)
        assert pmime.startswith("image/")
        assert Image.open(io.BytesIO(pair)).width == 200 + 210  # 两页横向拼接

        cover = adapter.cover(120)
        assert cover is not None and cover[0] and cover[1].startswith("image/")
    finally:
        adapter.close()


# ------------------------------------------------------------------ 工厂分流
def _fake_convert(epub_path: Path):
    data = epub_path.read_bytes()

    def fake_run(argv, **kwargs):
        out = Path(argv[2])
        # 契约：ebook-convert 按扩展名选输出插件，临时名必须以 .epub 结尾
        assert str(out).lower().endswith(".epub"), argv
        out.write_bytes(data)
        return SimpleNamespace(returncode=0, stderr=b"")

    return fake_run


def test_factory_routes_manga_to_comic_adapter(env, tmp_path: Path, monkeypatch):
    epub = _make_epub(tmp_path / "manga.epub", pages=10, text_per_page=3)
    monkeypatch.setattr(mobi_mod, "calibre_available", lambda: "ebook-convert")
    monkeypatch.setattr(mobi_mod.subprocess, "run", _fake_convert(epub))
    ref = _mobi_ref(tmp_path, "仁医01.mobi")
    adapter = mobi_mod.open_mobi_adapter(env, ref)
    try:
        assert isinstance(adapter, mobi_mod.MobiComicAdapter)
        assert isinstance(adapter, FixedAdapter)
        assert adapter.meta().mode == "fixed"
    finally:
        adapter.close()


def test_factory_keeps_text_mobi_flow(env, tmp_path: Path, monkeypatch):
    epub = _make_epub(tmp_path / "novel.epub", pages=8, text_per_page=400, title="文字书")
    monkeypatch.setattr(mobi_mod, "calibre_available", lambda: "ebook-convert")
    monkeypatch.setattr(mobi_mod.subprocess, "run", _fake_convert(epub))
    ref = _mobi_ref(tmp_path, "novel.mobi")
    adapter = mobi_mod.open_mobi_adapter(env, ref)
    try:
        assert isinstance(adapter, mobi_mod.MobiAdapter)
        assert not isinstance(adapter, FixedAdapter)
        meta = adapter.meta()
        assert meta.mode == "flow" and meta.format == "mobi"
    finally:
        adapter.close()
