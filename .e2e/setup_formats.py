"""为端到端验证补充 EPUB / PDF / FB2 三种格式（此前从未真正渲染过）。

用法：server\\.venv\\Scripts\\python.exe -X utf8 .e2e\\setup_formats.py
"""

from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
LIB = ROOT / "library"


def _cover_png(w: int = 600, h: int = 900, title: str = "EPUB") -> bytes:
    img = Image.new("RGB", (w, h), (47, 93, 98))
    draw = ImageDraw.Draw(img)
    draw.rectangle([30, 30, w - 30, h - 30], outline=(250, 248, 245), width=8)
    draw.rectangle([70, 200, w - 70, 260], fill=(250, 248, 245))
    draw.rectangle([70, 300, w - 130, 340], fill=(182, 221, 225))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def write_epub() -> Path:
    """手写一个合规的 EPUB3（同时覆盖 OPF/nav 解析与 zipfile 兜底路径）。"""
    folder = LIB / "EPUB"
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / "示例电子书.epub"

    chapters = []
    for i in range(1, 5):
        body = "".join(
            f"<p>这是第 {i} 章的第 {j} 段正文，用来验证 EPUB 解析、"
            f"块模型转换与分页渲染是否与 TXT 一致。</p>"
            for j in range(1, 9)
        )
        chapters.append(
            f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>第{i}章</title></head>
<body><h1>第{i}章 EPUB 测试</h1>{body}
<p><img src="../images/fig{i}.png" alt="fig"/></p>
</body></html>"""
        )

    manifest_items = "\n".join(
        f'    <item id="c{i}" href="text/ch{i}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(1, 5)
    )
    spine_items = "\n".join(f'    <itemref idref="c{i}"/>' for i in range(1, 5))
    nav_lis = "\n".join(
        f'      <li><a href="text/ch{i}.xhtml">第{i}章 EPUB 测试</a></li>' for i in range(1, 5)
    )

    opf = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:e2e-epub-0001</dc:identifier>
    <dc:title>示例电子书</dc:title>
    <dc:creator>端到端验证</dc:creator>
    <dc:language>zh-CN</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="cover" href="images/cover.png" media-type="image/png" properties="cover-image"/>
{manifest_items}
    <item id="f1" href="images/fig1.png" media-type="image/png"/>
    <item id="f2" href="images/fig2.png" media-type="image/png"/>
  </manifest>
  <spine>
{spine_items}
  </spine>
</package>
"""

    nav = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>目录</title></head><body>
<nav epub:type="toc"><ol>
{nav_lis}
</ol></nav></body></html>"""

    container = """<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""

    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        # mimetype 必须第一个且不压缩（EPUB 规范）
        zf.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        zf.writestr("META-INF/container.xml", container)
        zf.writestr("OEBPS/content.opf", opf)
        zf.writestr("OEBPS/nav.xhtml", nav)
        zf.writestr("OEBPS/images/cover.png", _cover_png())
        zf.writestr("OEBPS/images/fig1.png", _cover_png(400, 300, "fig1"))
        zf.writestr("OEBPS/images/fig2.png", _cover_png(300, 400, "fig2"))
        for i, html in enumerate(chapters, 1):
            zf.writestr(f"OEBPS/text/ch{i}.xhtml", html)
    return target


def write_pdf() -> Path:
    """用 Pillow 写一个多页 PDF（pypdfium2 直渲路径）。"""
    folder = LIB / "PDF"
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / "示例文档.pdf"
    pages = []
    for i in range(1, 6):
        img = Image.new("RGB", (1240, 1754), (255, 255, 255))  # A4 @150dpi
        draw = ImageDraw.Draw(img)
        draw.rectangle([60, 60, 1180, 1694], outline=(47, 93, 98), width=4)
        draw.rectangle([120, 160, 900, 230], fill=(47, 93, 98))
        for row in range(14):
            y = 320 + row * 90
            draw.rectangle([120, y, 1120 - (row % 4) * 120, y + 40], fill=(205, 210, 210))
        draw.ellipse([520, 1500, 720, 1640], fill=(122, 92, 70))
        pages.append(img)
    pages[0].save(target, save_all=True, append_images=pages[1:], format="PDF", resolution=150.0)
    return target


def write_fb2() -> Path:
    folder = LIB / "FB2"
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / "示例fb2.fb2"
    sections = []
    for i in range(1, 4):
        body = "".join(
            f"<p>这是第 {i} 节第 {j} 段正文，用于验证 FB2 的 XML → 块模型转换。</p>"
            for j in range(1, 7)
        )
        sections.append(f"<section><title><p>第{i}节 FB2 测试</p></title>{body}</section>")
    target.write_text(
        f"""<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
  <description>
    <title-info>
      <book-title>示例 FB2</book-title>
      <author><first-name>端到端</first-name><last-name>验证</last-name></author>
      <lang>zh</lang>
    </title-info>
  </description>
  <body>
    <title><p>示例 FB2</p></title>
    {''.join(sections)}
  </body>
</FictionBook>""",
        encoding="utf-8",
    )
    return target


def main() -> int:
    for maker in (write_epub, write_pdf, write_fb2):
        path = maker()
        print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
