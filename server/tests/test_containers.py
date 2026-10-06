"""容器适配器：条目名映射、zip 炸弹守卫、单张图片图集（设计 10.3：漫画页序 / zip-slip）。"""

from __future__ import annotations

import io
import os
import sys
import types
import zipfile
from pathlib import Path

import pytest

from app.formats.base import AssetNotFoundError, ParseError, UnsupportedFormatError
from app.formats.comics import (
    ComicAdapter,
    FolderComicContainer,
    RarComicContainer,
    SevenZipComicContainer,
    TarComicContainer,
    ZipComicContainer,
    list_folder_images,
    open_container,
    rar_backend_tool,
)
from app.formats.registry import create_adapter
from app.formats.base import BookRef
from tests.conftest import jpeg_bytes, png_bytes, write_cbz


# --------------------------------------------------------------- 单张图片
def test_open_container_accepts_single_image(env, tmp_path: Path):
    """单张图片 = 单页图集。

    回归测试：``GalleryService.register`` 允许把单张图片注册成"单页图集"
    （浏览页的"打开为图集"入口），但 ``open_container`` 只认目录与压缩包，
    于是注册成功之后每一次 /meta、/layout、/pages 都是 415。
    """
    image = tmp_path / "solo.jpg"
    image.write_bytes(jpeg_bytes(300, 400))

    container = open_container(env, image)
    try:
        assert isinstance(container, FolderComicContainer)
        assert container.names == ["solo.jpg"]
        assert container.read("solo.jpg")[:3] == b"\xff\xd8\xff"
    finally:
        container.close()


def test_single_image_adapter_end_to_end(env, tmp_path: Path):
    image = tmp_path / "solo.png"
    image.write_bytes(png_bytes(120, 200))
    ref = BookRef(
        book_id="gallery000000001",
        vpath="/lib/solo.png",
        path=image,
        format="gallery",
        virtual=True,
    )
    adapter = create_adapter(env, ref)
    try:
        assert adapter.mode == "fixed"
        assert adapter.page_count() == 1
        data, mime = adapter.page_bytes(1, viewport_w=100)
        assert data and mime.startswith("image/")
        meta = adapter.meta()
        assert meta.page_count == 1
    finally:
        adapter.close()


def test_single_image_container_ignores_non_image(env, tmp_path: Path):
    other = tmp_path / "notes.txt"
    other.write_text("hi")
    container = FolderComicContainer(env, other)
    try:
        assert container.names == []
    finally:
        container.close()


# --------------------------------------------------------------- 名称映射
def test_tar_duplicate_separator_entry_is_readable(env, tmp_path: Path):
    """TAR 的 `./` / `dir//x` 条目同样需要 norm → raw 映射。"""
    import tarfile

    archive = tmp_path / "book.cbt"
    payload = png_bytes(20, 30)
    with tarfile.open(archive, "w") as tf:
        info = tarfile.TarInfo("./dir//01.png")
        info.size = len(payload)
        tf.addfile(info, io.BytesIO(payload))

    container = TarComicContainer(env, archive)
    try:
        assert container.names == ["dir/01.png"]
        assert container.read("dir/01.png")[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        container.close()


def _install_fake_rarfile(monkeypatch, entries, *, tool="unrar", raise_on_tool_setup=False):
    """装一个最小可用的 rarfile 替身。

    关键在于 `open()` 只接受**容器内的原始条目名**（rarfile 的 NameToInfo 就是
    精确字典查找）—— 这正是 RAR 容器缺失 norm→raw 映射时会暴露的行为。
    """

    class _Info:
        def __init__(self, name, data):
            self.filename = name
            self.file_size = len(data)

        def is_dir(self):
            return False

    class _RarFile:
        def __init__(self, path):
            self._data = dict(entries)
            self._infos = [_Info(name, data) for name, data in entries.items()]

        def infolist(self):
            return list(self._infos)

        def open(self, name):
            if name not in self._data:
                raise FileNotFoundError(f"No such file: {name!r}")
            return io.BytesIO(self._data[name])

        def close(self):
            pass

    def _tool_setup():
        if raise_on_tool_setup:
            raise RuntimeError("Cannot find working tool")
        return tool

    module = types.ModuleType("rarfile")
    module.RarFile = _RarFile
    module.tool_setup = _tool_setup
    monkeypatch.setitem(sys.modules, "rarfile", module)
    return module


def test_rar_maps_normalized_name_back_to_raw(env, monkeypatch):
    """RAR 列表用规范化名，读取必须映射回容器内的原始名。

    回归测试：`read()` 以前直接 `self._rf.open(norm)`。对 `./dir/01.png` 这类
    条目，规范化名 `dir/01.png` 在 rarfile 的精确查找表里根本不存在 ——
    列表正常，每一页读取都失败。
    """
    entries = {
        "./dir/01.png": png_bytes(20, 30),
        "dir//02.png": png_bytes(20, 30),
        "plain.png": png_bytes(20, 30),
    }
    _install_fake_rarfile(monkeypatch, entries)

    container = RarComicContainer(env, Path("fake.cbr"))
    try:
        assert container.names == ["dir/01.png", "dir/02.png", "plain.png"]
        for name in container.names:
            assert container.read(name)[:8] == b"\x89PNG\r\n\x1a\n", name
    finally:
        container.close()


def test_rar_unknown_entry_raises_asset_not_found(env, monkeypatch):
    _install_fake_rarfile(monkeypatch, {"a.png": png_bytes(10, 10)})
    container = RarComicContainer(env, Path("fake.cbr"))
    try:
        with pytest.raises(AssetNotFoundError):
            container.read("missing.png")
    finally:
        container.close()


def test_rar_missing_backend_tool_is_unsupported_not_crash(env, monkeypatch):
    """rarfile 4.4+ 找不到解压后端时**抛异常**而不是返回 None。

    旧代码只判断返回值，异常会一路冒到通用处理器 → 500，而契约要求 415 + 原因。
    """
    _install_fake_rarfile(monkeypatch, {"a.png": png_bytes(10, 10)}, raise_on_tool_setup=True)
    import rarfile

    assert rar_backend_tool(rarfile) is None
    with pytest.raises(UnsupportedFormatError) as excinfo:
        RarComicContainer(env, Path("fake.cbr"))
    assert excinfo.value.reason == "需安装 unrar"


def test_rar_backend_tool_returns_none_when_setup_returns_none():
    class _Mod:
        @staticmethod
        def tool_setup():
            return None

    assert rar_backend_tool(_Mod) is None


# --------------------------------------------------------------- 炸弹守卫
def test_zip_entry_size_limit_is_enforced(env, tmp_path: Path, make_config):
    cfg = make_config(limits={"max_archive_entry_mb": 1, "max_archive_total_mb": 64})
    env.config = cfg
    archive = tmp_path / "big.cbz"
    write_cbz(archive, {"huge.png": b"\x00" * (2 * 1024 * 1024), "ok.png": png_bytes(10, 10)})
    container = ZipComicContainer(env, archive)
    try:
        assert "huge.png" in container.names
        with pytest.raises(ParseError):
            container.read("huge.png")
        # 正常条目不受影响
        assert container.read("ok.png")[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        container.close()


def test_zip_total_size_limit_is_enforced(env, tmp_path: Path, make_config):
    cfg = make_config(limits={"max_archive_entry_mb": 64, "max_archive_total_mb": 1})
    env.config = cfg
    archive = tmp_path / "many.cbz"
    write_cbz(
        archive,
        {f"p{i}.png": b"\x00" * (512 * 1024) for i in range(4)},
    )
    with pytest.raises(ParseError):
        ZipComicContainer(env, archive)


def test_read_stream_does_not_trust_declared_size(env):
    """只信压缩包头里声明的尺寸是不够的：流式读取必须强制上限。"""
    container = ZipComicContainer.__new__(ZipComicContainer)
    container.env = env
    container.patterns = []
    container.max_entry_bytes = 16
    container.max_total_bytes = 0

    with pytest.raises(ParseError):
        container._read_stream(io.BytesIO(b"x" * 1000), "liar.png")
    assert container._read_stream(io.BytesIO(b"x" * 10), "ok.png") == b"x" * 10


def test_read_stream_unlimited_when_zero(env):
    container = ZipComicContainer.__new__(ZipComicContainer)
    container.max_entry_bytes = 0
    container.max_total_bytes = 0
    assert container._read_stream(io.BytesIO(b"x" * 5000), "a") == b"x" * 5000


def test_epub_entry_limit_is_enforced(env, tmp_path: Path, make_config):
    """EPUB 走同一个 zipfile，压缩包守卫必须同样覆盖它（否则 2GB 条目直接打爆内存）。"""
    cfg = make_config(limits={"max_archive_entry_mb": 1, "max_archive_total_mb": 64})
    env.config = cfg

    from app.formats.base import BookRef
    from app.formats.epub import EpubAdapter

    archive = tmp_path / "bomb.epub"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr("META-INF/container.xml", "<container/>")
        zf.writestr("OEBPS/huge.xhtml", "<html><body>" + "x" * (2 * 1024 * 1024) + "</body></html>")

    ref = BookRef(book_id="epubbomb0000001", vpath="/lib/bomb.epub", path=archive, format="epub")
    adapter = EpubAdapter(env, ref)
    try:
        with pytest.raises(ParseError):
            adapter._read_entry("OEBPS/huge.xhtml")
        # 小条目正常
        assert adapter._read_entry("mimetype") == b"application/epub+zip"
    finally:
        adapter.close()


# --------------------------------------------------------------- 其它容器
def test_folder_container_rejects_escape(env, tmp_path: Path):
    root = tmp_path / "gallery"
    root.mkdir()
    (root / "a.png").write_bytes(png_bytes(10, 10))
    outside = tmp_path / "outside.png"
    outside.write_bytes(png_bytes(10, 10))

    container = FolderComicContainer(env, root)
    try:
        assert container.names == ["a.png"]
        with pytest.raises(AssetNotFoundError):
            container.read("../outside.png")
    finally:
        container.close()


def test_folder_container_recursive(env, tmp_path: Path):
    root = tmp_path / "gallery"
    (root / "sub").mkdir(parents=True)
    (root / "a.png").write_bytes(png_bytes(10, 10))
    (root / "sub" / "b.png").write_bytes(png_bytes(10, 10))

    flat = FolderComicContainer(env, root, recursive=False)
    deep = FolderComicContainer(env, root, recursive=True)
    try:
        assert flat.names == ["a.png"]
        assert deep.names == ["a.png", "sub/b.png"]
    finally:
        flat.close()
        deep.close()


def test_folder_container_does_not_stat_every_file(env, tmp_path: Path, monkeypatch):
    """图集可能有上万张图：构造容器不得对每张图 stat（否则每请求都卡文件系统）。

    回归：``FolderComicContainer`` 曾在构造时把全部条目 ``getsize`` 存进字典。
    """
    root = tmp_path / "gallery"
    root.mkdir()
    for i in range(5):
        (root / f"{i}.png").write_bytes(png_bytes(4, 4))

    calls = {"n": 0}
    real_getsize = os.path.getsize

    def counting_getsize(path):
        calls["n"] += 1
        return real_getsize(path)

    monkeypatch.setattr(os.path, "getsize", counting_getsize)

    container = FolderComicContainer(env, root)
    try:
        assert len(container.names) == 5
        assert calls["n"] == 0  # 构造阶段零 stat
        assert container.size_of("0.png") > 0
        assert calls["n"] == 1  # 仅显式索取尺寸时才 stat 一次
    finally:
        container.close()


def test_folder_listing_cache_refreshes_on_mtime_change(tmp_path: Path):
    """目录 mtime 变化（增删图片）必须立即刷新缓存，不等 TTL 过期。"""
    root = tmp_path / "gallery"
    root.mkdir()
    (root / "a.png").write_bytes(png_bytes(4, 4))
    assert list_folder_images(root, False, []) == ["a.png"]

    (root / "b.png").write_bytes(png_bytes(4, 4))
    # 显式把目录 mtime 往前推，避免某些文件系统的时间戳精度导致 mtime 不变
    before = os.stat(root).st_mtime
    os.utime(root, (before + 5, before + 5))
    assert list_folder_images(root, False, []) == ["a.png", "b.png"]


def test_sevenzip_missing_dependency_is_unsupported(env, tmp_path: Path, monkeypatch):
    monkeypatch.setitem(sys.modules, "py7zr", None)
    archive = tmp_path / "x.cb7"
    archive.write_bytes(b"not a real 7z")
    with pytest.raises((UnsupportedFormatError, ParseError)):
        SevenZipComicContainer(env, archive)


def _write_cb7(archive: Path, pages: dict) -> None:
    """写入 7z；py7zr 0.x 与 1.x 的 writestr 参数顺序不同（1.x 是 data, arcname）。"""
    import py7zr

    arc = py7zr.SevenZipFile(str(archive), "w")
    try:
        for name, data in pages.items():
            try:
                arc.writestr(name, data)
            except TypeError:
                arc.writestr(data, name)
    finally:
        arc.close()


def test_sevenzip_container_reads_entries(env, tmp_path: Path):
    """CB7 条目读取必须对已安装的 py7zr 版本可用。

    回归：py7zr 1.x 删除了 ``SevenZipFile.read()/readall()``，导致 CB7 能列出
    （layout/contents 正常）但取页一律 422 PARSE_FAILED（LESSONS 2026-10-06）。
    """
    import io

    archive = tmp_path / "comic.cb7"
    pages = {f"p{i:02d}.png": png_bytes(60, 90) for i in range(1, 4)}
    _write_cb7(archive, pages)

    container = SevenZipComicContainer(env, archive)
    try:
        assert container.names == ["p01.png", "p02.png", "p03.png"]
        payload = container.read("p01.png")
        assert payload[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        container.close()

    ref = BookRef(
        book_id="cb70000000000001", vpath="/lib/comic.cb7", path=archive, format="cb7"
    )
    adapter = ComicAdapter(env, ref)
    try:
        assert adapter.page_count() == 3
        aspect = adapter.page_aspect(1)
        assert aspect == pytest.approx(90 / 60, rel=0.01)
        data, mime = adapter.page_bytes(2, viewport_w=300)
        assert data and mime.startswith("image/")
    finally:
        adapter.close()


def test_adapter_page_count_and_order(env, tmp_path: Path, cbz_factory):
    archive = cbz_factory(tmp_path / "comic.cbz", count=5, width=400, height=600)
    ref = BookRef(book_id="cbz0000000000001", vpath="/lib/comic.cbz", path=archive, format="cbz")
    adapter = ComicAdapter(env, ref)
    try:
        assert adapter.page_count() == 5
        assert adapter.meta().page_count == 5
        assert adapter.meta().mode == "fixed"
        # 自然排序：page_001 … page_005
        assert adapter._entry(1) == "page_001.png"
        aspect = adapter.page_aspect(1)
        assert aspect == pytest.approx(600 / 400, rel=0.01)
        data, mime = adapter.page_bytes(1, viewport_w=200)
        assert data and mime.startswith("image/")
    finally:
        adapter.close()


def test_adapter_rejects_out_of_range_page(env, tmp_path: Path, cbz_factory):
    archive = cbz_factory(tmp_path / "comic.cbz", count=2)
    ref = BookRef(book_id="cbz0000000000002", vpath="/lib/comic.cbz", path=archive, format="cbz")
    adapter = ComicAdapter(env, ref)
    try:
        with pytest.raises(UnsupportedFormatError):
            adapter.page_bytes(99)
    finally:
        adapter.close()


def test_strip_pages_cover_full_height(env, tmp_path: Path):
    """条漫切片必须覆盖整张长图，且重叠不能逼近视口高度。"""
    root = tmp_path / "strip"
    root.mkdir()
    (root / "long.png").write_bytes(png_bytes(600, 5000))
    ref = BookRef(book_id="gallery000000002", vpath="/lib/strip", path=root, format="gallery", virtual=True)
    adapter = ComicAdapter(env, ref)
    try:
        slices = adapter.strip_pages(1000, 0)
        assert len(slices) == 5
        assert slices[0].y0 == 0
        assert slices[-1].is_last_of_image
        covered = max(s.y0 + s.height for s in slices)
        assert covered == 5000
        # 重叠过大时步进会退化，必须被夹住
        heavy = adapter.strip_pages(1000, 5000)
        assert len(heavy) <= 5 * 2
    finally:
        adapter.close()
