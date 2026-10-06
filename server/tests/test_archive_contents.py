"""压缩包内容浏览（设计 7.6）：容器浏览模式 + contents API + asset 端点。

覆盖：
- 容器 browse()：全量条目（含非图片）、目录合成、跳过规则与 zip-slip 名过滤；
- read_entry()：任意条目读取与缺失报错；
- archive_info() / asset()（adapter 级）；
- HTTP：/books/{id}/contents 列表 / 分页 / 导航、meta 概览、asset 预览、
  非压缩包书 415、server/info 能力位。
"""

from __future__ import annotations

import io
import tarfile
import zipfile
from pathlib import Path

import pytest

from app.formats.base import AssetNotFoundError, BookRef
from app.formats.comics import ZipComicContainer
from tests.conftest import jpeg_bytes, png_bytes, write_cbz

API = "/api/v1"


def _inner_zip_bytes() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("a.txt", "inside")
        zf.writestr("p1.png", png_bytes(50, 50))
    return buf.getvalue()


def _mixed_entries() -> dict:
    return {
        "vol01/001.png": png_bytes(300, 400),
        "vol01/002.png": png_bytes(300, 400),
        "vol02/001.jpg": jpeg_bytes(300, 400),
        "novel.txt": "第一章 测试\n正文内容".encode("utf-8"),
        "inner.zip": _inner_zip_bytes(),
        "__MACOSX/vol01/._001.png": b"junk",
        "Thumbs.db": b"junk",
        ".hidden.png": png_bytes(10, 10),
    }


def _make_mixed_zip(path: Path) -> Path:
    return write_cbz(path, _mixed_entries())


@pytest.fixture
def archive_env(tmp_books: Path, make_config):
    """带 .zip include 的配置 + 混合内容压缩包 + 已扫描书库的 TestClient。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    cfg = make_config(
        library={"include_ext": [".txt", ".epub", ".pdf", ".cbz", ".zip", ".7z", ".tar"]}
    )
    _make_mixed_zip(tmp_books / "mixed.zip")
    (tmp_books / "plain.txt").write_text("普通书", encoding="utf-8")
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        yield client


def _book_id(client, name: str) -> str:
    data = client.get(f"{API}/files", params={"path": "/lib"}).json()
    for entry in data["entries"]:
        if entry["name"] == name:
            assert entry["book_id"], f"{name} 缺少 book_id"
            return entry["book_id"]
    raise AssertionError(f"{name} 不在目录列表里: {[e['name'] for e in data['entries']]}")


# --------------------------------------------------------------- 容器 browse()
def test_browse_lists_all_entries_and_synthesizes_dirs(env, tmp_books: Path):
    path = _make_mixed_zip(tmp_books / "b.zip")
    with ZipComicContainer(env, path) as c:
        entries = c.browse()
    names = [e.name for e in entries]
    # 全量条目（含非图片）
    assert "novel.txt" in names and "inner.zip" in names
    # 目录按文件条目合成（zip 里没有显式目录记录）
    assert "vol01" in names and "vol02" in names
    assert all(e.is_dir for e in entries if e.name in ("vol01", "vol02"))
    # 跳过规则与隐藏条目不可见
    assert not any("__MACOSX" in n for n in names)
    assert "Thumbs.db" not in names and ".hidden.png" not in names
    # 排序：目录与文件混排，自然排序
    assert names.index("vol01") < names.index("vol01/001.png")


def test_browse_tar(env, tmp_books: Path):
    path = tmp_books / "b.tar"
    with tarfile.open(path, "w") as tf:
        raw = b"hello"
        info = tarfile.TarInfo("docs/readme.txt")
        info.size = len(raw)
        tf.addfile(info, io.BytesIO(raw))
        png = png_bytes(20, 20)
        info2 = tarfile.TarInfo("docs/img.png")
        info2.size = len(png)
        tf.addfile(info2, io.BytesIO(png))
    from app.formats.comics import TarComicContainer

    with TarComicContainer(env, path) as c:
        names = [e.name for e in c.browse()]
    assert "docs" in names and "docs/readme.txt" in names and "docs/img.png" in names
    with TarComicContainer(env, path) as c:
        assert c.read_entry("docs/readme.txt") == b"hello"


def test_read_entry_any_file_and_missing(env, tmp_books: Path):
    path = _make_mixed_zip(tmp_books / "r.zip")
    with ZipComicContainer(env, path) as c:
        assert c.read_entry("novel.txt") == _mixed_entries()["novel.txt"]
        # 规范化后的名字也能读
        assert c.read_entry("./vol01/001.png") == _mixed_entries()["vol01/001.png"]
        with pytest.raises(AssetNotFoundError):
            c.read_entry("nope.txt")
        with pytest.raises(AssetNotFoundError):
            c.read_entry("../../etc/passwd")


# --------------------------------------------------------------- adapter 级
def test_adapter_archive_info_and_asset(env, tmp_books: Path):
    from app.formats import registry

    path = _make_mixed_zip(tmp_books / "a.zip")
    ref = BookRef(
        book_id="atest0000000001",
        vpath="/lib/a.zip",
        path=path,
        format="zip",
        size=path.stat().st_size,
    )
    adapter = registry.create_adapter(env, ref)
    try:
        info = adapter.archive_info()
        assert info is not None
        assert info["entry_count"] == 5, "5 个文件条目（图片 3 + txt + inner.zip）"
        assert info["image_count"] == 3
        data, mime = adapter.asset("vol01/001.png")
        assert data == _mixed_entries()["vol01/001.png"]
        assert mime.startswith("image/")
        with pytest.raises(AssetNotFoundError):
            adapter.asset("missing.bin")
    finally:
        adapter.close()


# --------------------------------------------------------------- HTTP 契约
def test_meta_exposes_archive_summary(archive_env):
    bid = _book_id(archive_env, "mixed.zip")
    body = archive_env.get(f"{API}/books/{bid}/meta").json()
    assert body["archive"] is not None
    assert body["archive"]["entry_count"] == 5
    assert body["archive"]["image_count"] == 3
    assert body["archive"]["depth"] == 0
    # 普通书没有 archive 块
    plain = _book_id(archive_env, "plain.txt")
    assert archive_env.get(f"{API}/books/{plain}/meta").json()["archive"] is None


def test_contents_root_lists_dirs_and_files(archive_env):
    bid = _book_id(archive_env, "mixed.zip")
    resp = archive_env.get(f"{API}/books/{bid}/contents")
    assert resp.status_code == 200
    body = resp.json()
    assert body["entry"] == "" and body["parent"] is None
    assert body["archive"]["entry_count"] == 5
    by_name = {e["name"]: e for e in body["entries"]}
    # 目录
    assert by_name["vol01"]["type"] == "dir"
    assert by_name["vol01"]["gallery"]["image_count"] == 2
    assert by_name["vol01"]["gallery"]["can_open_as_book"] is True
    # 可打开的书
    assert by_name["novel.txt"]["type"] == "book"
    assert by_name["novel.txt"]["format"] == "txt"
    assert by_name["novel.txt"]["openable"] is True
    # 嵌套压缩包
    assert by_name["inner.zip"]["type"] == "book"
    assert by_name["inner.zip"]["format"] == "zip"
    # 跳过规则同样适用于 contents
    assert "Thumbs.db" not in by_name
    # 根列表只列直接子级：子目录里的图片要进子目录才可见
    assert "vol02/001.jpg" not in by_name and "vol01/001.png" not in by_name
    # 子目录里的图片条目：类型 / 预览 URL
    sub = archive_env.get(f"{API}/books/{bid}/contents", params={"entry": "vol02"}).json()
    img = next(e for e in sub["entries"] if e["name"] == "001.jpg")
    assert img["type"] == "image"
    assert img["preview_url"], "图片条目要有预览 URL"


def test_contents_pagination_and_navigation(archive_env):
    bid = _book_id(archive_env, "mixed.zip")
    page1 = archive_env.get(f"{API}/books/{bid}/contents", params={"limit": 2}).json()
    assert page1["total"] == 4
    assert len(page1["entries"]) == 2
    page2 = archive_env.get(
        f"{API}/books/{bid}/contents", params={"limit": 2, "offset": 2}
    ).json()
    assert len(page2["entries"]) == 2
    names = [e["name"] for e in page1["entries"]] + [e["name"] for e in page2["entries"]]
    assert len(set(names)) == 4, "分页不得重复/遗漏"
    # 进入子目录
    sub = archive_env.get(f"{API}/books/{bid}/contents", params={"entry": "vol01"}).json()
    assert sub["entry"] == "vol01"
    assert sub["parent"] == ""
    assert {e["name"] for e in sub["entries"]} == {"001.png", "002.png"}
    assert sub["archive"]["image_count"] == 3, "概览始终是整包统计"


def test_contents_missing_dir_and_missing_book(archive_env):
    bid = _book_id(archive_env, "mixed.zip")
    resp = archive_env.get(f"{API}/books/{bid}/contents", params={"entry": "nope"})
    assert resp.status_code == 404
    resp = archive_env.get(f"{API}/books/deadbeefdeadbeef/contents")
    assert resp.status_code == 404


def test_contents_on_non_archive_book_is_unsupported(archive_env):
    bid = _book_id(archive_env, "plain.txt")
    resp = archive_env.get(f"{API}/books/{bid}/contents")
    assert resp.status_code == 415


def test_asset_endpoint_serves_entry_bytes(archive_env):
    bid = _book_id(archive_env, "mixed.zip")
    listing = archive_env.get(
        f"{API}/books/{bid}/contents", params={"entry": "vol01"}
    ).json()
    preview = next(e for e in listing["entries"] if e["name"] == "001.png")["preview_url"]
    resp = archive_env.get(preview)
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("image/")
    assert resp.content == _mixed_entries()["vol01/001.png"]
    # 缺失条目 404
    resp = archive_env.get(f"{API}/books/{bid}/asset", params={"path": "nope.png"})
    assert resp.status_code == 404


def test_server_info_advertises_archive_browse(archive_env):
    body = archive_env.get(f"{API}/server/info").json()
    assert body["features"]["archive_browse"] is True
