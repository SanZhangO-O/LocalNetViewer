"""EPUB / FB2 适配器回归（此前从未真正渲染过，一跑就暴露了两个缺陷）。

- EPUB 的 `<img>` 内联图片会让解析抛 `NameError: STYLE_NONE` → 整个 /layout 500；
- FB2 的书名来自 `<book-title>`，但代码把 lxml 元素当真值用（叶子节点为假值），
  于是书名退化成文件名。
"""

from __future__ import annotations

import zipfile
from pathlib import Path

import pytest

from app.formats.base import BookRef
from app.formats.epub import EpubAdapter
from app.formats.fb2 import Fb2Adapter
from tests.conftest import png_bytes


# ------------------------------------------------------------------ EPUB
def _make_epub(path: Path, *, with_image: bool, empty_body: bool = False) -> Path:
    body = "" if empty_body else "<p>第一段正文。</p><p>第二段正文。</p>"
    image = '<p><img src="images/fig.png" alt="f"/></p>' if with_image else ""
    html = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>'
        f"<body><h1>第一章</h1>{body}{image}</body></html>"
    )
    opf = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="i">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="i">urn:uuid:test</dc:identifier>
    <dc:title>测试书名</dc:title><dc:creator>作者名</dc:creator><dc:language>zh</dc:language>
  </metadata>
  <manifest>
    <item id="c1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="f1" href="images/fig.png" media-type="image/png"/>
  </manifest>
  <spine><itemref idref="c1"/></spine>
</package>"""
    container = """<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf"
    media-type="application/oebps-package+xml"/></rootfiles></container>"""
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", container)
        zf.writestr("OEBPS/content.opf", opf)
        zf.writestr("OEBPS/text/ch1.xhtml", html)
        zf.writestr("OEBPS/images/fig.png", png_bytes(120, 90))
    return path


def _adapter(env, path: Path) -> EpubAdapter:
    ref = BookRef(book_id="epubtest00000001", vpath=f"/lib/{path.name}", path=path, format="epub")
    return EpubAdapter(env, ref)


def test_epub_with_inline_image_parses(env, tmp_path: Path):
    """回归：`<img>` 会让 `_spans` 走到 `STYLE_NONE`，未导入时直接 NameError → 500。"""
    path = _make_epub(tmp_path / "img.epub", with_image=True)
    adapter = _adapter(env, path)
    try:
        doc = adapter.document()
        assert doc.chapters, "至少要有一章"
        types = [b.type for ch in doc.chapters for b in ch.blocks]
        assert "image" in types, "内联图片应当成为 image 块"
        assert "paragraph" in types
    finally:
        adapter.close()


def test_epub_without_image_still_works(env, tmp_path: Path):
    path = _make_epub(tmp_path / "plain.epub", with_image=False)
    adapter = _adapter(env, path)
    try:
        doc = adapter.document()
        assert doc.chapters
        assert all(b.type != "image" for ch in doc.chapters for b in ch.blocks)
    finally:
        adapter.close()


def test_epub_empty_body_does_not_crash(env, tmp_path: Path):
    """空 <body> 是 BeautifulSoup Tag 的假值场景，必须仍然正常返回。"""
    path = _make_epub(tmp_path / "empty.epub", with_image=False, empty_body=True)
    adapter = _adapter(env, path)
    try:
        doc = adapter.document()
        assert doc.chapters is not None
    finally:
        adapter.close()


def test_epub_meta_title_and_chapters(env, tmp_path: Path):
    path = _make_epub(tmp_path / "meta.epub", with_image=False)
    adapter = _adapter(env, path)
    try:
        meta = adapter.meta()
        assert meta.title == "测试书名"
        assert meta.author == "作者名"
        assert meta.language == "zh"
        assert meta.mode == "flow"
        assert len(meta.chapters) == 1
    finally:
        adapter.close()


def test_epub_image_asset_is_served(env, tmp_path: Path):
    path = _make_epub(tmp_path / "asset.epub", with_image=True)
    adapter = _adapter(env, path)
    try:
        data, mime = adapter.asset("images/fig.png")
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        assert mime == "image/png"
    finally:
        adapter.close()


# ------------------------------------------------------------------ FB2
FB2 = """<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
  <description>
    <title-info>
      <book-title>示例 FB2 书名</book-title>
      <author><first-name>张</first-name><last-name>三</last-name></author>
      <lang>zh</lang>
      <coverpage><image xlink:href="#cover.png"
        xmlns:xlink="http://www.w3.org/1999/xlink"/></coverpage>
    </title-info>
  </description>
  <body>
    <title><p>正文标题</p></title>
    <section><title><p>第一节</p></title><p>第一节正文。</p></section>
    <section><title><p>第二节</p></title><p>第二节正文。</p><empty-line/>
      <image xlink:href="#fig.png" xmlns:xlink="http://www.w3.org/1999/xlink"/></section>
  </body>
  <binary id="cover.png" content-type="image/png">%s</binary>
  <binary id="fig.png" content-type="image/png">%s</binary>
</FictionBook>"""


def _make_fb2(path: Path) -> Path:
    import base64

    png = base64.b64encode(png_bytes(80, 60)).decode("ascii")
    path.write_text(FB2 % (png, png), encoding="utf-8")
    return path


def _fb2(env, path: Path) -> Fb2Adapter:
    ref = BookRef(book_id="fb2test000000001", vpath=f"/lib/{path.name}", path=path, format="fb2")
    return Fb2Adapter(env, ref)


def test_fb2_title_comes_from_book_title(env, tmp_path: Path):
    """回归：`<book-title>` 是叶子节点，lxml 真值为 False。

    旧代码写成 `first_local(info, "book-title") or info` 与三元条件，把"找到了"
    误判成"没找到"，书名退化成文件名。
    """
    path = _make_fb2(tmp_path / "示例fb2.fb2")
    adapter = _fb2(env, path)
    try:
        meta = adapter.meta()
        assert meta.title == "示例 FB2 书名", f"书名退化成了 {meta.title!r}"
        assert meta.author == "张 三"
        assert meta.language == "zh"
        doc = adapter.document()
        assert doc.meta["title"] == "示例 FB2 书名"
    finally:
        adapter.close()


def test_fb2_sections_and_blocks(env, tmp_path: Path):
    path = _make_fb2(tmp_path / "sections.fb2")
    adapter = _fb2(env, path)
    try:
        meta = adapter.meta()
        assert [c["title"] for c in meta.chapters] == ["第一节", "第二节"]
        doc = adapter.document()
        assert len(doc.chapters) == 2
        blocks = doc.chapters[1].blocks
        assert any(b.type == "separator" for b in blocks), "empty-line 应成为分隔块"
        assert any(b.type == "image" for b in blocks), "内嵌图片应成为 image 块"
        assert all(b.text.strip() for b in blocks if b.is_text)
    finally:
        adapter.close()


def test_fb2_cover_from_coverpage(env, tmp_path: Path):
    path = _make_fb2(tmp_path / "cover.fb2")
    adapter = _fb2(env, path)
    try:
        data, mime = adapter.cover(0)
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        assert mime == "image/png"
    finally:
        adapter.close()


def test_fb2_missing_book_title_falls_back_to_filename(env, tmp_path: Path):
    path = tmp_path / "无书名.fb2"
    path.write_text(
        '<?xml version="1.0" encoding="utf-8"?>'
        '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
        "<description><title-info><lang>zh</lang></title-info></description>"
        "<body><section><p>正文</p></section></body></FictionBook>",
        encoding="utf-8",
    )
    adapter = _fb2(env, path)
    try:
        assert adapter.meta().title == "无书名"
    finally:
        adapter.close()


def test_fb2_broken_xml_is_parse_error(env, tmp_path: Path):
    from app.formats.base import ParseError

    path = tmp_path / "broken.fb2"
    path.write_bytes(b"<FictionBook><body><section>")
    adapter = _fb2(env, path)
    try:
        with pytest.raises(ParseError):
            adapter.meta()
    finally:
        adapter.close()
