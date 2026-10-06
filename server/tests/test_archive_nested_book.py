"""包内嵌套书（设计 7.6）：contents/open → meta/layout/pages 闭环。

覆盖：
- zip 内 txt 打开为嵌套书：阅读闭环、幂等注册、进度键独立；
- 单张图片 → 单页书；目录 → 前缀图集；
- 嵌套压缩包（包中包）：作为漫画书打开、继续 contents 浏览、再打开更深层条目；
- 深度上限、缺失/不支持条目的错误契约；
- 注册表重启后仍可解析；父压缩包内容变化后旧嵌套 id 失效。
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path

import pytest

from app.formats.archivebook import nested_book_id, sanitize_fs_name
from tests.conftest import png_bytes, write_cbz

API = "/api/v1"

_NOVEL = (
    ("第一章 开始\n" + "这是第一章的正文内容，用来产生足够的行数。\n" * 40)
    + ("\n第二章 继续\n" + "这是第二章的正文内容，用来产生足够的行数。\n" * 40)
)


def _inner_zip_bytes() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("deep.txt", "包中包里的文本，" * 200)
        zf.writestr("ip.png", png_bytes(60, 90))
    return buf.getvalue()


def _archive_entries() -> dict:
    return {
        "books/novel.txt": _NOVEL.encode("utf-8"),
        "manga/001.png": png_bytes(300, 450),
        "manga/002.png": png_bytes(300, 450),
        "single.png": png_bytes(120, 180),
        "inner/inner.zip": _inner_zip_bytes(),
        "data.bin": b"\x00\x01\x02",
    }


def _write_archive(path: Path, entries: dict) -> Path:
    return write_cbz(path, entries)


@pytest.fixture
def api(tmp_books: Path, make_config):
    from fastapi.testclient import TestClient

    from app.main import create_app

    cfg = make_config(library={"include_ext": [".txt", ".cbz", ".zip"]})
    _write_archive(tmp_books / "pack.zip", _archive_entries())
    (tmp_books / "novel.txt").write_text("库里的普通书", encoding="utf-8")
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        yield client


def _book_id(client, name: str) -> str:
    data = client.get(f"{API}/files", params={"path": "/lib"}).json()
    for entry in data["entries"]:
        if entry["name"] == name:
            return entry["book_id"]
    raise AssertionError(f"{name} 不在目录列表里")


def _open(client, bid: str, entry: str):
    return client.post(f"{API}/books/{bid}/contents/open", json={"entry": entry})


def _layout_and_page(client, bid: str) -> None:
    layout = client.post(
        f"{API}/books/{bid}/layout",
        json={
            "viewport": {"w": 800, "h": 1200},
            "margin": {"left": 20, "right": 20, "top": 20, "bottom": 60},
            "font": {"family": "auto", "size_px": 40, "line_height": 1.6},
            "paragraph": {"spacing_px": 10, "first_line_indent_chars": 2},
            "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
            "chapter_new_page": True,
            "mode": "flow",
        },
    )
    assert layout.status_code == 200, layout.text
    layout_id = layout.json()["layout_id"]
    page = client.get(f"{API}/books/{bid}/pages/1", params={"layout_id": layout_id})
    assert page.status_code == 200, page.text
    assert page.headers["X-Total-Pages"] >= "1"


# --------------------------------------------------------------- 文本条目
def test_open_txt_inside_zip_full_loop(api):
    bid = _book_id(api, "pack.zip")
    resp = _open(api, bid, "books/novel.txt")
    assert resp.status_code == 200, resp.text
    body = resp.json()
    nested = body["book_id"]
    assert body["kind"] == "book" and body["format"] == "txt" and body["mode"] == "flow"
    assert body["depth"] == 1
    assert nested != bid

    meta = api.get(f"{API}/books/{nested}/meta")
    assert meta.status_code == 200
    mbody = meta.json()
    assert mbody["format"] == "txt" and mbody["mode"] == "flow"
    assert mbody["title"] == "novel"
    assert mbody["source"] == "/lib/pack.zip/books/novel.txt"
    assert mbody["archive"] is None

    _layout_and_page(api, nested)
    # 幂等：重复 open 返回同一 book_id
    again = _open(api, bid, "books/novel.txt").json()
    assert again["book_id"] == nested


def test_open_missing_entry_and_unsupported_type(api):
    bid = _book_id(api, "pack.zip")
    resp = _open(api, bid, "books/none.txt")
    assert resp.status_code == 404
    resp = _open(api, bid, "data.bin")
    assert resp.status_code == 415
    resp = _open(api, bid, "../escape.txt")
    assert resp.status_code in (400, 404), "穿越路径不允许打开"


def test_open_single_image_inside_zip(api):
    bid = _book_id(api, "pack.zip")
    body = _open(api, bid, "single.png").json()
    nested = body["book_id"]
    assert body["kind"] == "image" and body["page_count"] == 1
    meta = api.get(f"{API}/books/{nested}/meta").json()
    assert meta["mode"] == "fixed" and meta["page_count"] == 1
    cover = api.get(f"{API}/books/{nested}/cover")
    assert cover.status_code == 200
    assert cover.headers["content-type"].startswith("image/")


def test_open_dir_inside_zip_as_gallery(api):
    bid = _book_id(api, "pack.zip")
    body = _open(api, bid, "manga").json()
    nested = body["book_id"]
    assert body["kind"] == "dir" and body["format"] == "gallery"
    assert body["page_count"] == 2 and body["mode"] == "fixed"
    meta = api.get(f"{API}/books/{nested}/meta").json()
    assert meta["mode"] == "fixed" and meta["page_count"] == 2
    layout = api.post(
        f"{API}/books/{nested}/layout",
        json={
            "viewport": {"w": 800, "h": 1200},
            "margin": {"left": 0, "right": 0, "top": 0, "bottom": 0},
            "font": {"family": "auto", "size_px": 40, "line_height": 1.6},
            "paragraph": {"spacing_px": 0, "first_line_indent_chars": 0},
            "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
            "chapter_new_page": True,
            "mode": "fixed",
        },
    )
    assert layout.status_code == 200
    layout_id = layout.json()["layout_id"]
    assert layout.json()["total_pages"] == 2
    p1 = api.get(f"{API}/books/{nested}/pages/1", params={"layout_id": layout_id})
    assert p1.status_code == 200 and p1.headers["content-type"].startswith("image/")


# --------------------------------------------------------------- 包中包
def test_nested_zip_open_browse_and_deep_book(api):
    bid = _book_id(api, "pack.zip")
    # 1) 内层 zip 作为漫画书打开（解压后走常规容器）
    body = _open(api, bid, "inner/inner.zip").json()
    inner_bid = body["book_id"]
    assert body["kind"] == "book" and body["format"] == "zip" and body["mode"] == "fixed"
    meta = api.get(f"{API}/books/{inner_bid}/meta").json()
    assert meta["mode"] == "fixed" and meta["page_count"] == 1
    assert meta["archive"]["depth"] == 1, "meta 概览要带嵌套深度"

    # 2) 继续浏览内层 zip 的内容
    listing = api.get(f"{API}/books/{inner_bid}/contents").json()
    names = {e["name"] for e in listing["entries"]}
    assert names == {"deep.txt", "ip.png"}
    assert listing["archive"]["depth"] == 1

    # 3) 再往里打开文本：链长 2（pack.zip → inner.zip → deep.txt）
    deep = _open(api, inner_bid, "deep.txt").json()
    assert deep["depth"] == 2
    deep_meta = api.get(f"{API}/books/{deep['book_id']}/meta")
    assert deep_meta.status_code == 200
    assert deep_meta.json()["source"] == "/lib/pack.zip/inner/inner.zip/deep.txt"
    _layout_and_page(api, deep["book_id"])


def test_depth_limit_blocks_deeper_open(tmp_books: Path, make_config):
    from fastapi.testclient import TestClient

    from app.main import create_app

    cfg = make_config(
        library={"include_ext": [".txt", ".cbz", ".zip"]}, archive={"max_depth": 1}
    )
    _write_archive(tmp_books / "pack.zip", _archive_entries())
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        bid = _book_id(client, "pack.zip")
        inner = _open(client, bid, "inner/inner.zip").json()
        resp = _open(client, inner["book_id"], "deep.txt")
        assert resp.status_code == 415


# --------------------------------------------------------------- 持久化与失效
def test_registry_survives_restart(tmp_books: Path, make_config):
    from app.context import AppContext

    cfg = make_config(library={"include_ext": [".txt", ".cbz", ".zip"]})
    _write_archive(tmp_books / "pack.zip", _archive_entries())

    ctx1 = AppContext(cfg)
    ctx1.library.scan_blocking()
    ref = ctx1.library.book_by_vpath("/lib/pack.zip")
    assert ref is not None
    info = ctx1.archives.open_nested(ref, "books/novel.txt")
    nested = info["book_id"]
    # 模拟重启：全新上下文加载同一缓存目录
    ctx2 = AppContext(cfg)
    ctx2.library.load_index()
    resolved = ctx2.library.book_by_id(nested)
    assert resolved is not None, "重启后嵌套书 id 必须仍可解析"
    assert resolved.extra["chain"] == ["books/novel.txt"]
    adapter = ctx2.create_adapter(resolved)
    try:
        assert adapter.meta().mode == "flow"
    finally:
        adapter.close()


def test_parent_change_invalidates_nested_id(tmp_books: Path, make_config):
    from fastapi.testclient import TestClient

    from app.main import create_app

    cfg = make_config(library={"include_ext": [".txt", ".cbz", ".zip"]})
    path = _write_archive(tmp_books / "pack.zip", _archive_entries())
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        bid = _book_id(client, "pack.zip")
        nested = _open(client, bid, "books/novel.txt").json()["book_id"]
        meta = client.get(f"{API}/books/{nested}/meta")
        assert meta.status_code == 200

        # 压缩包内容变化（尺寸也变化）→ 重新扫描后旧父 id 与嵌套 id 全部失效
        entries = _archive_entries()
        entries["books/novel.txt"] = ("内容已经完全不同\n" * 500).encode("utf-8")
        _write_archive(path, entries)
        app.state.ctx.library.scan_blocking()
        resp = client.get(f"{API}/books/{nested}/meta")
        assert resp.status_code == 404, "父压缩包替换后旧嵌套 id 必须失效"
        # 解压缓存也被清理
        assert not any(
            p.is_file() for p in cfg.extract_cache_dir.rglob("novel.txt")
        ), "父压缩包变化后链上解压缓存必须被删除"


# --------------------------------------------------------------- 单元
def test_nested_book_id_deterministic_and_entry_sensitive():
    a = nested_book_id("parent123", "books/novel.txt")
    b = nested_book_id("parent123", "./books/novel.txt/")
    c = nested_book_id("parent124", "books/novel.txt")
    d = nested_book_id("parent123", "books/other.txt")
    assert a == b, "规范化后同一条目必须同 id"
    assert a != c and a != d
    assert len(a) == 16 and all(ch in "0123456789abcdef" for ch in a)


def test_sanitize_fs_name_blocks_reserved_and_illegal():
    assert sanitize_fs_name("con.txt") == "_con.txt"
    assert sanitize_fs_name('a<b>:c?.png') == "a_b__c_.png"
    assert sanitize_fs_name("dir/name.zip") == "name.zip"
    assert sanitize_fs_name("...") == "file"
