"""M5 格式扩展的回归用例（对抗性 review 修复，设计 7.7）。

对应 LESSONS.md 2026-10-07：能力判定与打开同源、容器/图集页白名单、
嗅探遵守列表契约、RTF 不吞字面反斜杠、超线性解析修复。
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path

from PIL import Image

from app.formats import registry
from app.formats.base import BookRef
from app.formats.rtfbook import rtf_to_text
from tests.conftest import png_bytes, write_cbz

API = "/api/v1"

_FB2 = """<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
<description><title-info><book-title>ZipBook</book-title></title-info></description>
<body><section><title><p>S1</p></title><p>text</p></section></body></FictionBook>""".encode("utf-8")


def _book_ref(path: Path, fmt: str) -> BookRef:
    return BookRef(
        book_id="m5" + "0" * 14,
        vpath=f"/lib/{path.name}",
        path=path,
        format=fmt,
        size=path.stat().st_size,
    )


def _multiframe_tiff() -> bytes:
    buf = io.BytesIO()
    frames = [
        Image.new("RGB", (30, 40), (255, 0, 0)),
        Image.new("RGB", (30, 40), (0, 255, 0)),
    ]
    frames[0].save(buf, format="TIFF", save_all=True, append_images=frames[1:])
    return buf.getvalue()


# --------------------------------------------------------------- RTF
def test_rtf_keeps_literal_backslash():
    """后处理正则曾把正文里的字面反斜杠当控制字删掉（C:\\new → C:）。"""
    text = rtf_to_text(b"{\\rtf1\\ansi C:\\\\new\\\\share done \\par line2}")
    assert "C:\\new\\share done" in text, text
    assert "line2" in text


def test_rtf_parses_large_input_quickly():
    """解析不得是 O(n²)：控制字密集的 ~2MB 输入必须亚秒级。"""
    import time

    big = "{\\rtf1 " + "\\u20013\\u25991" * 60000 + "}"
    start = time.time()
    rtf_to_text(big.encode("latin-1"))
    assert time.time() - start < 2.0


# --------------------------------------------------------------- 容器页白名单
def test_container_keeps_tiff_pages(tmp_books: Path, env):
    """M5 把独立 .tif 改为多页格式后，压缩包内的 TIFF 页不得被丢弃。"""
    cbz = write_cbz(
        tmp_books / "pages.cbz",
        {"p1.tif": _multiframe_tiff(), "p2.png": png_bytes(20, 20)},
    )
    adapter = registry.create_adapter(env, _book_ref(cbz, "cbz"))
    try:
        assert adapter.meta().page_count == 2, "TIFF 页被当作非图片丢弃了"
    finally:
        adapter.close()


# --------------------------------------------------------------- 代码文本
def test_code_adapter_preserves_indent_and_chapter_consistency(tmp_books: Path, env):
    path = tmp_books / "snippet.py"
    lines = ["# 注释"] + [f"x{i} = {i}" for i in range(9000)]  # ~90KB，跨越分节阈值
    path.write_text("\n".join(lines), encoding="utf-8")
    adapter = registry.create_adapter(env, _book_ref(path, "code"))
    try:
        meta = adapter.meta()
        doc = adapter.document()
        texts = [b.text for ch in doc.chapters for b in ch.blocks if b.type == "paragraph"]
        assert any(t.startswith("x1 = 1") for t in texts), "代码缩进/行文本被破坏"
        # meta 与 document 必须同一判据（曾经 _scan_chapters 没走 _clean 钩子）
        assert [c["title"] for c in meta.chapters] == [ch.title for ch in doc.chapters]
        assert len(meta.chapters) == len(doc.chapters)
    finally:
        adapter.close()


# --------------------------------------------------------------- HTTP 列表契约
def test_fb2_zip_is_listed_by_default(tmp_books: Path, config):
    """.fb2.zip 是复合扩展名：include_ext 门禁必须用 ext_key_of 判定。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    with zipfile.ZipFile(tmp_books / "novel.fb2.zip", "w") as zf:
        zf.writestr("book.fb2", _FB2)

    app = create_app(config, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        body = client.get(f"{API}/files", params={"path": "/lib"}).json()
    entry = next(e for e in body["entries"] if e["name"] == "novel.fb2.zip")
    assert entry["type"] == "book" and entry["format"] == "fbz"


def test_sniffed_entries_respect_listing_contract(tmp_books: Path, make_config):
    """嗅探出的条目：show_image_files=false 时图片隐藏；total 与实际条目一致。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    (tmp_books / "hidden.xyz").write_bytes(png_bytes(20, 20))  # 错扩展名图片
    (tmp_books / "shown.png").write_bytes(png_bytes(20, 20))  # 已知图片
    (tmp_books / "blob.bin").write_bytes(bytes(range(256)) * 4)  # 未知二进制
    (tmp_books / "note.txt").write_text("正文", encoding="utf-8")

    cfg = make_config(gallery={"show_image_files": False})
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        body = client.get(f"{API}/files", params={"path": "/lib"}).json()
    names = {e["name"] for e in body["entries"]}
    assert "shown.png" not in names, "show_image_files=false 时已知图片不得列出"
    assert "hidden.xyz" not in names, "show_image_files=false 时嗅探出的图片不得列出"
    assert "blob.bin" not in names, "未知二进制不得列出"
    assert body["total"] == len(body["entries"]), "total 必须与实际下发条目一致"


def test_sniffed_unknown_image_listed_when_enabled(tmp_books: Path):
    """默认配置下，错扩展名的图片应被嗅探为可打开的图集条目。"""
    from fastapi.testclient import TestClient

    from app.config import AppConfig, LibraryRootCfg
    from app.main import create_app

    (tmp_books / "photo.xyz").write_bytes(png_bytes(20, 20))
    cfg = AppConfig()
    cfg.base_dir = tmp_books.parent
    cfg.library.roots = [LibraryRootCfg(name="lib", path=str(tmp_books))]
    cfg.cache.dir = str(tmp_books.parent / "cache")
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        body = client.get(f"{API}/files", params={"path": "/lib"}).json()
    entry = next(e for e in body["entries"] if e["name"] == "photo.xyz")
    assert entry["type"] == "image" and entry["openable"] is True
