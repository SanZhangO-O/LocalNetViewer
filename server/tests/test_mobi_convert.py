"""MOBI 转换（Calibre）回归测试：转换临时产物必须保持 .epub 扩展名。

背景（DESIGN.md 13 / v0.4.2）：``ebook-convert`` 按输出文件的扩展名选择输出插件，
把临时文件命名为 ``{book_id}.epub.tmp`` 会让真实书籍的每次转换必然失败
（``ValueError: No plugin to handle output format: tmp``），且静态测试发现不了。
本文件不要求安装 Calibre：用假的 ``subprocess.run`` 模拟其"按扩展名拒绝未知格式"
的行为，钉住"传给 ebook-convert 的输出路径必须以 .epub 结尾"这一契约。
"""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from app.formats import mobi as mobi_mod
from app.formats.base import BookRef


def _ref(tmp_path: Path) -> BookRef:
    src = tmp_path / "book000000000001.mobi"
    src.write_bytes(b"fake mobi")
    return BookRef(
        book_id="book000000000001",
        vpath="/lib/book000000000001.mobi",
        path=src,
        format="mobi",
        size=src.stat().st_size,
        mtime=src.stat().st_mtime,
    )


def test_convert_output_path_keeps_epub_extension(env, tmp_path, monkeypatch):
    target = tmp_path / "converted" / "book000000000001.epub"
    seen: dict = {}

    def fake_run(argv, **kwargs):
        seen["argv"] = list(argv)
        out = argv[2]
        # 模拟 Calibre：输出格式由扩展名决定，未知扩展名 → exit=1
        if not str(out).lower().endswith(".epub"):
            return SimpleNamespace(
                returncode=1,
                stderr=b"ValueError: No plugin to handle output format: tmp",
            )
        Path(out).write_bytes(b"fake epub")
        return SimpleNamespace(returncode=0, stderr=b"")

    monkeypatch.setattr(mobi_mod, "calibre_available", lambda: "ebook-convert")
    monkeypatch.setattr(mobi_mod.subprocess, "run", fake_run)

    fake_self = SimpleNamespace(env=env)
    mobi_mod.MobiAdapter._convert(fake_self, _ref(tmp_path), target)

    assert seen["argv"][0] == "ebook-convert"
    assert seen["argv"][2].lower().endswith(".epub"), seen["argv"]
    assert target.is_file() and target.stat().st_size > 0
    # 临时文件已被原子 replace 掉，converted 目录不残留半成品
    assert not list(target.parent.glob("*.tmp*"))


def test_convert_failure_cleans_tmp_and_reports(monkeypatch, tmp_path, env):
    target = tmp_path / "converted" / "book000000000002.epub"

    def fake_run(argv, **kwargs):
        out = Path(argv[2])
        out.write_bytes(b"broken")  # 模拟半成品
        return SimpleNamespace(returncode=1, stderr=b"boom")

    monkeypatch.setattr(mobi_mod, "calibre_available", lambda: "ebook-convert")
    monkeypatch.setattr(mobi_mod.subprocess, "run", fake_run)

    with pytest.raises(mobi_mod.ParseError, match="Calibre 转换失败"):
        mobi_mod.MobiAdapter._convert(SimpleNamespace(env=env), _ref(tmp_path), target)

    assert not target.exists()
    assert not list(target.parent.glob("*.tmp*"))
