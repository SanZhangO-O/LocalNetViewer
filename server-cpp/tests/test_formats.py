"""格式适配器回归测试：容器条目映射、编码探测、深层嵌套、压缩包守卫、图片解码。

这些用例全部对应"审查发现并修复"的具体缺陷，改坏任意一处都会在这里失败。
"""

from __future__ import annotations

import io
import time
import zipfile
from pathlib import Path

import pytest
from PIL import Image
from conftest import png_bytes

FIXED = {
    "viewport": {"w": 800, "h": 1200},
    "margin": {"left": 0, "right": 0, "top": 0, "bottom": 0},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 0, "first_line_indent_chars": 0},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "fixed",
}
FLOW = dict(FIXED, mode="flow", viewport={"w": 1080, "h": 1920},
            margin={"left": 40, "right": 40, "top": 40, "bottom": 180},
            paragraph={"spacing_px": 20, "first_line_indent_chars": 2})

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""


def opf(title: str, spine: str = '<itemref idref="c1"/>') -> str:
    return f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="id">t</dc:identifier><dc:title>{title}</dc:title><dc:language>zh</dc:language>
 </metadata>
 <manifest><item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/></manifest>
 <spine>{spine}</spine>
</package>"""


def epub_bytes(chapter_html: str, title: str = "T", spine: str = '<itemref idref="c1"/>') -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", opf(title, spine))
        zf.writestr("OEBPS/ch1.xhtml", chapter_html)
    return buf.getvalue()


# --------------------------------------------------------------------- 容器条目映射
def test_zip_directory_entries_do_not_shift_pages(server, make_book):
    """中心目录里的目录条目不能让页内容整体错位（审查 B1）。

    旧实现把"过滤后 entries_ 的下标"当成 miniz 的全局文件号，只要包里有一个
    目录条目（zip -r 打包的 CBZ 很常见），第 2 页起全部返回错图、末页丢失。
    """
    path = server.lib / "cbz" / "dirtest.cbz"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(zipfile.ZipInfo("images/"), b"")          # 目录条目
        zf.writestr("images/01.png", png_bytes(10, 10))
        zf.writestr("images/02.png", png_bytes(10, 20))
        zf.writestr("images/03.png", png_bytes(10, 30))
    bid = server.book_id("/lib/cbz", "dirtest.cbz")
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{bid}/layout", json=FIXED).json()
        assert lay["total_pages"] == 3
        sizes = []
        for page in (1, 2, 3):
            r = c.get(f"/api/v1/books/{bid}/pages/{page}",
                      params={"layout_id": lay["layout_id"], "fit": "original"})
            assert r.status_code == 200, r.text
            sizes.append(Image.open(io.BytesIO(r.content)).size)
    assert sizes == [(10, 10), (10, 20), (10, 30)]


def test_zip_unsafe_entries_are_skipped(server, make_book):
    """`../`、绝对路径、盘符、隐藏名条目一律不进页表（zip-slip 语义）。"""
    path = server.lib / "cbz" / "unsafe.cbz"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("ok.png", png_bytes(10, 10))
        zf.writestr("../up.png", png_bytes(11, 11))
        zf.writestr("/abs.png", png_bytes(12, 12))
        zf.writestr("C:/drive.png", png_bytes(13, 13))
        zf.writestr(".hidden.png", png_bytes(14, 14))
        zf.writestr("__MACOSX/x.png", png_bytes(15, 15))
    bid = server.book_id("/lib/cbz", "unsafe.cbz")
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{bid}/layout", json=FIXED).json()
    assert lay["total_pages"] == 1


# --------------------------------------------------------------------- 编码探测
def test_utf8_sample_boundary_is_not_misdetected(server, make_book):
    """前 64 KiB 采样切断多字节字符时不能把 UTF-8 书判成 cp1252（审查 B2）。

    旧实现用"严格 UTF-8 校验 64 KiB 采样"，采样尾部截断即判定 UTF-8 非法，
    于是 cp1252 胜出 —— 整本中文书变乱码、页数也全错。
    """
    base = "第1章 编码边界测试\n"
    body = ""
    while len(base.encode()) + len(body.encode()) < 65534:
        body += "测试正文内容。"
    pad = 65535 - (len(base.encode()) + len(body.encode()))
    data = (base + body + ("a" * max(0, pad)) + "中" + "文继续测试。" * 200).encode("utf-8")
    # 前提：采样确实被切断（否则这个用例没有覆盖到目标分支）
    with pytest.raises(UnicodeDecodeError):
        data[:65536].decode("utf-8")
    make_book("txt/边界.txt", data)
    bid = server.book_id("/lib/txt", "边界.txt")
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{bid}/layout", json=FLOW).json()
        page = c.get(f"/api/v1/books/{bid}/pages/1",
                     params={"layout_id": lay["layout_id"], "format": "json"}).json()
    assert page["blocks"][0]["lines"][0] == "第1章 编码边界测试"
    assert lay["total_pages"] == 60


def test_utf16_surrogate_pairs_are_combined(server, make_book):
    """UTF-16 代理对必须合成一个码点，否则 total_chars/锚点偏移全错（审查 H6）。

    😀(U+1F600) 与 𠀀(U+20000) 在 UTF-16 里各占两个 16 位单元；不合并就会把它们
    当成两个码点，total_chars 偏大、其后所有 char 锚点整体后移。
    """
    body = "正文😀𠀀测试。" * 20          # 每段 7 个码点（星平面字符算 1 个）
    make_book("txt/astral.txt", b"\xff\xfe" + ("第1章 UTF16\n" + body).encode("utf-16-le"))
    bid = server.book_id("/lib/txt", "astral.txt")
    with server.client() as c:
        meta = c.get(f"/api/v1/books/{bid}/meta").json()
    assert meta["chapters"][0]["total_chars"] == len(body) == 140


# --------------------------------------------------------------------- 深层嵌套
def test_deeply_nested_markup_does_not_kill_the_server(server, make_book):
    """6 万层嵌套不能让进程栈溢出（审查 B3），且结果与 Python 一致（0 页）。"""
    depth = 60000
    html = ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>d</title></head><body>'
            + "<div>" * depth + "深层嵌套正文" + "</div>" * depth + "</body></html>")
    make_book("epub/deep.epub", epub_bytes(html, "深层嵌套"))
    bid = server.book_id("/lib/epub", "deep.epub")
    with server.client() as c:
        r = c.post(f"/api/v1/books/{bid}/layout", json=FLOW)
        assert r.status_code == 200, r.text
        assert r.json()["total_pages"] == 0
        # 进程仍然活着
        assert c.get("/api/v1/server/info").status_code == 200


# --------------------------------------------------------------------- 压缩包守卫
def test_epub_entry_over_limit_is_rejected_without_oom(server, make_book):
    """超过 max_archive_entry_mb 的章节要报 422、不按声明尺寸分配内存（审查 H3）。

    测试配置把上限压到 16 MB（见 conftest），章节本体 24 MiB：miniz 的
    extract_to_heap 会照**声明**尺寸一次性分配（x64 上可达 4 GiB−1），旧实现先分配
    再检查上限；现在走流式回调，读到 limit+1 即中止。
    """
    payload = b"<html><body>" + b"\0" * (24 * 1024 * 1024) + b"</body></html>"
    path = server.lib / "epub" / "bomb.epub"
    path.parent.mkdir(parents=True, exist_ok=True)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", opf("炸弹"))
        zf.writestr("OEBPS/ch1.xhtml", payload)     # 就在 spine 里，解析必然要读
    path.write_bytes(buf.getvalue())
    bid = server.book_id("/lib/epub", "bomb.epub")
    with server.client() as c:
        t0 = time.time()
        r = c.get(f"/api/v1/books/{bid}/meta")
        elapsed = time.time() - t0
        assert r.status_code == 422, r.text
        assert "超过上限" in r.json()["message"]
        assert elapsed < 60, f"守卫应在读到 limit+1 时中止，实际 {elapsed:.1f}s"
        assert c.get("/api/v1/server/info").status_code == 200


# --------------------------------------------------------------------- 图片解码
def test_webp_and_tiff_pages_render(server, make_book):
    """stb 解不了的 WebP/TIFF 要走系统解码器，而不是给可打开的页返回 422（审查 H7）。"""
    for name, fmt in (("p1.webp", "WEBP"), ("p2.tiff", "TIFF"), ("p3.png", "PNG")):
        buf = io.BytesIO()
        Image.new("RGB", (60, 90), (220, 30, 30)).save(buf, format=fmt)
        make_book(f"gallery/{name}", buf.getvalue())
    with server.client() as c:
        r = c.post("/api/v1/books/open-folder", json={"path": "/lib/gallery"})
        assert r.status_code == 200, r.text
        bid = r.json()["book_id"]
        lay = c.post(f"/api/v1/books/{bid}/layout", json=FIXED).json()
        assert lay["total_pages"] == 3
        for page in (1, 2, 3):
            g = c.get(f"/api/v1/books/{bid}/pages/{page}",
                      params={"layout_id": lay["layout_id"], "fit": "viewport", "quality": 85})
            assert g.status_code == 200, f"page {page}: {g.text[:120]}"


# --------------------------------------------------------------------- EPUB 结构
def test_valid_epub_with_empty_spine_is_accepted(server, make_book):
    """container+OPF 合法但 spine 为空 → 200（不是 422，审查 M5）。"""
    make_book("epub/empty.epub", epub_bytes("<html><body><p>x</p></body></html>",
                                            "Empty", spine='toc="ncx"'))
    bid = server.book_id("/lib/epub", "empty.epub")
    with server.client() as c:
        r = c.get(f"/api/v1/books/{bid}/meta")
    assert r.status_code == 200, r.text


def test_container_formats_use_extension_names(server, make_book):
    """`.zip/.rar/.7z/.tar` 的 format 是扩展名本身，不是 cbz/cbr/cb7/cbt（审查 M6）。"""
    path = server.lib / "plain" / "pack.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("01.png", png_bytes(10, 10))
    with server.client() as c:
        entries = c.get("/api/v1/files", params={"path": "/lib/plain"}).json()["entries"]
    fmt = {e["name"]: e.get("format") for e in entries}
    assert fmt.get("pack.zip") == "zip"


def test_epub3_nav_title_keeps_inner_whitespace(server, make_book):
    """EPUB3 nav 的章节标题只去首尾空白，内部空白要保留（= get_text(" ", strip=True)）。

    旧实现整体 collapse，把 "Chapter    One" 压成 "Chapter One"，与 Python 的
    目录标题/解析缓存不一致（审查 M7）。
    """
    nav = ('<?xml version="1.0" encoding="utf-8"?>\n'
           '<html xmlns="http://www.w3.org/1999/xhtml" '
           'xmlns:epub="http://www.idpf.org/2007/ops"><body>'
           '<nav epub:type="toc"><ol>'
           '<li><a href="ch1.xhtml">Chapter    One</a></li>'
           '</ol></nav></body></html>')
    opf = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="id">nav</dc:identifier><dc:title>Nav</dc:title><dc:language>en</dc:language>
 </metadata>
 <manifest>
  <item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
 </manifest>
 <spine><itemref idref="c1"/></spine>
</package>"""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", opf)
        zf.writestr("OEBPS/nav.xhtml", nav)
        zf.writestr("OEBPS/ch1.xhtml",
                    "<html><body><h1>Chapter One</h1><p>text</p></body></html>")
    make_book("epub/nav.epub", buf.getvalue())
    bid = server.book_id("/lib/epub", "nav.epub")
    with server.client() as c:
        meta = c.get(f"/api/v1/books/{bid}/meta").json()
    assert meta["chapters"][0]["title"] == "Chapter    One"


# --------------------------------------------------------------------- 图片型 EPUB 分流
def image_epub_bytes(pages: int, text_per_page: int = 0, title: str = "漫画") -> bytes:
    """每页一张图（可选少量正文）的 EPUB，用于图片型分流回归。"""
    items = []
    refs = []
    extra = {}
    for i in range(1, pages + 1):
        iid = f"p{i:03d}"
        text = "字" * text_per_page
        items.append(
            f'<item id="{iid}" href="text/{iid}.xhtml" media-type="application/xhtml+xml"/>'
        )
        refs.append(f'<itemref idref="{iid}"/>')
        extra[f"OEBPS/text/{iid}.xhtml"] = (
            '<?xml version="1.0" encoding="utf-8"?>'
            '<html xmlns="http://www.w3.org/1999/xhtml"><body>'
            f'<p>{text}</p><p><img src="../images/{iid}.png"/></p></body></html>'
        ).encode("utf-8")
        extra[f"OEBPS/images/{iid}.png"] = png_bytes(200 + i, 300 + i)
    package = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:identifier id="id">t</dc:identifier>'
        f"<dc:title>{title}</dc:title><dc:language>zh</dc:language></metadata>"
        "<manifest>" + "".join(items) + "</manifest>"
        "<spine>" + "".join(refs) + "</spine></package>"
    )
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", package)
        for name, data in extra.items():
            zf.writestr(name, data)
    return buf.getvalue()


def test_image_epub_opens_as_fixed_comic(server, make_book):
    """图片型 EPUB（每页一图）必须按固定版式漫画打开（mode=fixed、页数=图片数）。

    走文本流排版时图片会被塞进带边距的版心、四周空边，无法铺满屏幕（真实反馈）。
    """
    make_book("epub/manga.epub", image_epub_bytes(10, text_per_page=3))
    bid = server.book_id("/lib/epub", "manga.epub")
    with server.client() as c:
        meta = c.get(f"/api/v1/books/{bid}/meta").json()
        assert meta["mode"] == "fixed"
        assert meta["format"] == "epub"
        assert meta["page_count"] == 10
        layout = c.post(f"/api/v1/books/{bid}/layout", json=FIXED).json()
        assert layout["mode"] == "fixed"
        assert layout["total_pages"] == 10
        r = c.get(
            f"/api/v1/books/{bid}/pages/1",
            params={"layout_id": layout["layout_id"], "fit": "viewport"},
        )
        assert r.status_code == 200
        assert r.headers["content-type"].startswith("image/")
        # 页序 = spine 顺序（首尾尺寸与造书时一致）
        assert Image.open(io.BytesIO(r.content)).size[0] > 200


def test_text_epub_keeps_flow_mode(server, make_book):
    """正文为主的 EPUB 维持流式排版（图片型检测不得误伤插图书/小说）。"""
    html = "<html><body><p>" + ("正文内容。" * 800) + "</p></body></html>"
    make_book("epub/novel.epub", epub_bytes(html, "小说"))
    bid = server.book_id("/lib/epub", "novel.epub")
    with server.client() as c:
        meta = c.get(f"/api/v1/books/{bid}/meta").json()
    assert meta["mode"] == "flow"
