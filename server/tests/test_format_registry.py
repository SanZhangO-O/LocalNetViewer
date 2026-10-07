"""格式注册表回归（M5，设计 7.7）。

对应 LESSONS.md 2026-10-07：``specs.py`` 声明了扩展名、但容器/注册表没接上，
"支持某格式"就只停留在文档里（``.cbz3`` 曾经声明却打不开）。这些用例把
"声明 → 可判定 → 有容器 → 能力位/默认列表一致"钉死。
"""

from __future__ import annotations

import zipfile
from pathlib import Path

import pytest

from app.config import DEFAULT_INCLUDE_EXT
from app.formats import comics, detect, registry, specs


@pytest.mark.parametrize("spec", specs.FORMAT_SPECS, ids=lambda s: s.id)
def test_spec_exts_are_reachable(spec: specs.FormatSpec):
    """每个 spec 的每个扩展名都必须经 registry.format_of 回到该 spec。"""
    for ext in spec.exts:
        name = f"sample{ext}"
        assert registry.format_of(name) == spec.id, (name, spec.id)


def test_exts_are_owned_by_single_spec():
    """扩展名不得跨格式重复：registry 索引会静默覆盖（.rb 曾撞车）。

    specs._validate() 在 import 时已保证；这里再钉一层，防止有人只改测试
    或绕过校验改数据。
    """
    owners: dict[str, str] = {}
    for spec in specs.FORMAT_SPECS:
        for ext in spec.exts:
            assert ext not in owners, f"{ext} 同时属于 {owners[ext]} 与 {spec.id}"
            owners[ext] = spec.id


@pytest.mark.parametrize("spec", specs.FORMAT_SPECS, ids=lambda s: s.id)
def test_comic_specs_have_container(spec: specs.FormatSpec):
    """comic 适配器的扩展名必须都登记在容器集合里。

    之前 ``.cbz3`` 在 specs/registry 里声明为漫画，但 ``comics`` 的 zip 容器
    集合没有它 → 打开直接 415。这个用例把两处钉在一起。
    """
    if spec.adapter != "comic":
        return
    for ext in spec.exts:
        assert ext in comics.ARCHIVE_EXTS, (spec.id, ext)


def test_default_include_ext_is_derived_from_specs():
    exts = set(DEFAULT_INCLUDE_EXT)
    assert {".txt", ".epub", ".pdf", ".cbz", ".fb2", ".mobi", ".md", ".mp3"} <= exts
    # 通用压缩包默认不列出（避免书库里每个 zip 都被当成一本书）
    assert ".zip" not in exts and ".rar" not in exts and ".tar.gz" not in exts
    assert exts <= set(specs.all_exts())


def test_format_infos_cover_specs_and_gallery():
    infos = registry.format_infos()
    ids = [info["id"] for info in infos]
    assert len(ids) == len(set(ids)), ids
    assert len(infos) == len(specs.FORMAT_SPECS) + 1, "specs + gallery 伪条目"
    by_id = {info["id"]: info for info in infos}
    assert by_id["txt"]["mode"] == "flow"
    assert by_id["pdf"]["mode"] == "fixed"
    assert by_id["audio"]["mode"] == "", "媒体不是可翻阅模式"
    assert ".png" in by_id["gallery"]["exts"]


def test_media_classification():
    assert registry.is_media_format("audio") and registry.is_media_format("video")
    assert not registry.is_indexable_format("audio")
    assert registry.is_indexable_format("txt")
    assert registry.is_browsable_format("cbz") and registry.is_browsable_format("iso")
    assert not registry.is_browsable_format("txt")


def test_ext_overrides_roundtrip():
    try:
        registry.configure_overrides({".mybook": "epub", "nodot": "txt"})
        assert registry.format_of("x.mybook") == "epub"
        assert registry.format_of("x.nodot") == "txt"
        assert registry.is_override_ext(".mybook") is True
        assert registry.is_override_ext(".nope") is False
    finally:
        registry.configure_overrides({})


def test_image_exts_single_source():
    """图片基础扩展名以 specs 为单一事实源；TIFF 单独走多页格式。"""
    from app.util import imaging

    assert set(specs.BASE_IMAGE_EXTS) <= imaging.supported_image_exts()
    assert {".tif", ".tiff"} <= imaging.page_image_exts()
    assert ".tif" not in imaging.supported_image_exts(), "独立 TIFF 由 tiff 格式打开"


def test_detect_odf_and_xlsb_are_not_misclassified(tmp_path: Path):
    """ODS/ODP 不得当 ODT 文本；xlsb 不得当 xlsx（列出→点开 422 的误判链）。"""
    ods = tmp_path / "fake.dat"
    with zipfile.ZipFile(ods, "w") as zf:
        zf.writestr("mimetype", "application/vnd.oasis.opendocument.spreadsheet")
        zf.writestr("content.xml", "<x/>")
        zf.writestr("meta.xml", "<x/>")
    assert detect.detect_file(ods).kind == "none"

    xlsb = tmp_path / "fake2.dat"
    with zipfile.ZipFile(xlsb, "w") as zf:
        zf.writestr("[Content_Types].xml", "<Types/>")
        zf.writestr("xl/workbook.bin", b"\x00")
        zf.writestr("xl/worksheets/sheet1.bin", b"\x00")
    assert detect.detect_file(xlsb).format != "xlsx"


def test_detect_misnamed_zip(tmp_path: Path):
    path = tmp_path / "mystery.bin"
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("p1.png", b"fake")
    detected = detect.detect_file(path)
    assert detected.kind == "book" and detected.format == "zip"


def test_detect_office_containers(tmp_path: Path):
    def _zip(name: str, entries: dict) -> Path:
        path = tmp_path / name
        with zipfile.ZipFile(path, "w") as zf:
            for entry_name, data in entries.items():
                zf.writestr(entry_name, data)
        return path

    docx = _zip("fake.dat", {"[Content_Types].xml": "<Types/>", "word/document.xml": "<w:document/>"})
    assert detect.detect_file(docx).format == "docx"
    odt = _zip("fake2.dat", {"content.xml": "<x/>", "meta.xml": "<x/>"})
    assert detect.detect_file(odt).format == "odt"


def test_detect_text_and_binary(tmp_path: Path):
    text = tmp_path / "noext"
    text.write_bytes("你好，世界\n第二行\n".encode("utf-8"))
    assert detect.detect_file(text).kind == "text"
    binary = tmp_path / "blob.bin"
    binary.write_bytes(bytes(range(256)) * 4)
    assert detect.detect_file(binary).kind == "none"
