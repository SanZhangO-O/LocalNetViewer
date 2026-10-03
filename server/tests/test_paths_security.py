"""路径穿越 / zip-slip / 符号链接逃逸（设计 10.3：zip-slip / 路径穿越）。

安全模型（设计 9）的五段防线：
解码 → 规范化 → resolve → 包含校验 → 拒绝符号链接。
这里对每一段都写正反用例。
"""

from __future__ import annotations

import os
import zipfile
from pathlib import Path

import pytest

from app.formats.comics import (
    ZipComicContainer,
    is_image_name,
    should_skip_entry,
)
from app.util.paths import (
    PathError,
    TraversalError,
    normalize_asset_key,
    normalize_vpath,
    resolve_in_root,
    safe_asset_path,
)
from tests.conftest import png_bytes


# --------------------------------------------------------------- normalize_vpath
@pytest.mark.parametrize(
    "raw",
    [
        "/lib/../../etc/passwd",
        "..",
        "/..",
        "/lib/../..",
        "lib/../../../windows/win.ini",
    ],
)
def test_traversal_segments_are_rejected(raw):
    with pytest.raises(PathError):
        normalize_vpath(raw)


@pytest.mark.parametrize(
    "raw",
    [
        "/lib/C:/windows/system32",
        "C:/windows",
        "/lib/file.txt:stream",
        "/lib/aux",
        "/lib/CON",
        "/lib/lpt1.txt",
    ],
)
def test_windows_specific_paths_are_rejected(raw):
    with pytest.raises(PathError):
        normalize_vpath(raw)


def test_nul_byte_is_rejected():
    with pytest.raises(PathError):
        normalize_vpath("/lib/a\x00b")


def test_url_encoded_traversal_is_decoded_then_rejected():
    """攻击者会先编码：%2e%2e%2f 必须先解码再判断。"""
    with pytest.raises(PathError):
        normalize_vpath("/lib/%2e%2e%2f%2e%2e%2fetc/passwd")
    with pytest.raises(PathError):
        normalize_vpath("/lib/..%2f..%2fetc")


def test_backslash_is_normalized_then_rejected():
    with pytest.raises(PathError):
        normalize_vpath("..\\..\\etc\\passwd")


def test_normalization_collapses_separators_and_dots():
    assert normalize_vpath("//lib///a/./b//") == "/lib/a/b"
    assert normalize_vpath("lib/a") == "/lib/a"
    assert normalize_vpath("/") == "/"
    assert normalize_vpath("", allow_empty=True) == "/"


def test_empty_without_allow_empty_raises():
    with pytest.raises(PathError):
        normalize_vpath("")


# --------------------------------------------------------------- resolve_in_root
def test_resolve_in_root_stays_inside(tmp_path: Path):
    root = tmp_path / "root"
    (root / "sub").mkdir(parents=True)
    assert resolve_in_root(root, ["sub", "a.txt"]) == (root / "sub" / "a.txt").resolve()


def test_resolve_in_root_rejects_parent_segment(tmp_path: Path):
    root = tmp_path / "root"
    root.mkdir()
    with pytest.raises(TraversalError):
        resolve_in_root(root, ["..", "outside.txt"])


def test_resolve_in_root_handles_empty_parts(tmp_path: Path):
    root = tmp_path / "root"
    root.mkdir()
    assert resolve_in_root(root, []) == root.resolve()


@pytest.mark.skipif(
    not hasattr(os, "symlink"), reason="平台不支持符号链接"
)
def test_symlink_escape_is_rejected(tmp_path: Path):
    """书库里的软链接指向外部时必须被拒绝（不跟随符号链接）。"""
    root = tmp_path / "root"
    outside = tmp_path / "outside"
    root.mkdir()
    outside.mkdir()
    (outside / "secret.txt").write_text("secret")
    link = root / "escape"
    try:
        link.symlink_to(outside, target_is_directory=True)
    except (OSError, NotImplementedError):
        pytest.skip("当前环境不允许创建符号链接（Windows 需要开发者模式）")

    with pytest.raises(TraversalError):
        resolve_in_root(root, ["escape", "secret.txt"])


# --------------------------------------------------------------- safe_asset_path
@pytest.mark.parametrize(
    "raw",
    ["../secret", "a/../../b", "..", "a/../../../../etc/passwd"],
)
def test_asset_path_rejects_parent(raw):
    with pytest.raises(PathError):
        safe_asset_path(raw)


def test_asset_path_normalizes():
    assert safe_asset_path("OEBPS/Images/./a.png") == "OEBPS/Images/a.png"
    assert safe_asset_path("\\a\\b.png") == "a/b.png"
    with pytest.raises(PathError):
        safe_asset_path("")
    with pytest.raises(PathError):
        safe_asset_path(None)  # type: ignore[arg-type]


def test_normalize_asset_key_strips_dot_slash():
    assert normalize_asset_key("./a/b.png") == "a/b.png"
    assert normalize_asset_key("a\\b.png") == "a/b.png"


# --------------------------------------------------------------- zip-slip
@pytest.mark.parametrize(
    "name",
    [
        "../../evil.png",
        "../evil.png",
        "/absolute/evil.png",
        "a/../../evil.png",
    ],
)
def test_zip_slip_entries_are_skipped(tmp_path: Path, env, name: str):
    """条目名越出容器根的图片不能进入页集合（否则会被当页读出来）。"""
    archive = tmp_path / "slip.cbz"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr(name, png_bytes(10, 10))
        zf.writestr("ok.png", png_bytes(10, 10))
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["ok.png"]
    finally:
        container.close()


def test_zip_drive_letter_entry_is_skipped(tmp_path: Path, env):
    archive = tmp_path / "drive.cbz"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("C:/evil.png", png_bytes(10, 10))
        zf.writestr("ok.png", png_bytes(10, 10))
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["ok.png"]
    finally:
        container.close()


def test_zip_dot_slash_entry_is_listed_and_readable(tmp_path: Path, env):
    """`./` 前缀是合法条目名（zip / 7z / bsdtar 都会生成）。

    回归测试：`should_skip_entry` 曾把 `.` 也当隐藏段，导致整包图片被丢掉
    （书打开后"没有可显示的内容"）；列表用规范化名、读取必须映射回原始名，
    否则会出现"列表正常但每一页都读不出来"。
    """
    archive = tmp_path / "dot.cbz"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("./dir/01.png", png_bytes(20, 30))
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["dir/01.png"]
        data = container.read("dir/01.png")
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        container.close()


def test_zip_duplicate_separator_entry_is_readable(tmp_path: Path, env):
    """`dir//01.png` 规范化后是 `dir/01.png`，读取必须按原始名映射。"""
    archive = tmp_path / "dup.cbz"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("dir//01.png", png_bytes(20, 30))
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["dir/01.png"]
        assert container.read("dir/01.png")[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        container.close()


def test_zip_non_image_entries_are_ignored(tmp_path: Path, env):
    archive = tmp_path / "mixed.cbz"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("a.png", png_bytes(10, 10))
        zf.writestr("readme.txt", b"hello")
        zf.writestr("__MACOSX/._a.png", b"junk")
        zf.writestr(".hidden.png", png_bytes(10, 10))
        zf.writestr("Thumbs.db", b"junk")
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["a.png"]
    finally:
        container.close()


def test_zip_names_are_naturally_sorted(tmp_path: Path, env):
    archive = tmp_path / "sort.cbz"
    names = ["p10.png", "p2.png", "p1.png", "P20.png", "p3.png"]
    with zipfile.ZipFile(archive, "w") as zf:
        for n in names:
            zf.writestr(n, png_bytes(10, 10))
    container = ZipComicContainer(env, archive)
    try:
        assert container.names == ["p1.png", "p2.png", "p3.png", "p10.png", "P20.png"]
    finally:
        container.close()


# --------------------------------------------------------------- 工具
@pytest.mark.parametrize(
    "name, expected",
    [
        ("a.png", True),
        ("a.JPG", True),
        ("a.jpeg", True),
        ("a.webp", True),
        ("a.txt", False),
        ("a.png.exe", False),
        ("noext", False),
        ("", False),
    ],
)
def test_is_image_name(name, expected):
    assert is_image_name(name) is expected


@pytest.mark.parametrize(
    "name, expected",
    [
        ("a.png", False),
        ("__MACOSX/a.png", True),
        (".hidden/a.png", True),
        ("a/.hidden.png", True),
        ("Thumbs.db", True),
        ("desktop.ini", True),
        (".DS_Store", True),
        ("", True),
    ],
)
def test_should_skip_entry(name, expected):
    assert should_skip_entry(name, ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]) is expected


def test_should_skip_entry_custom_glob():
    assert should_skip_entry("cover.jpg", ["cover.*"]) is True
    assert should_skip_entry("x/cover.jpg", ["cover.*"]) is True
    assert should_skip_entry("x/page.jpg", ["cover.*"]) is False
