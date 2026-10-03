"""字体发现与中文渲染（回归：默认字体退化成无中文字形的字体）。

这里用 fontTools 直接读字体 cmap 作为**独立判据**，而不是复用被测代码的结论 ——
否则"探测实现坏了"会让测试和实现一起错，测试就永远通不过（或者永远通过）。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from app.layout.fonts import (
    FontFace,
    FontManager,
    _name_suggests_cjk,
    _rasterize_glyph,
    is_wide_char,
)

CJK_CODEPOINT = 0x4E2D  # 中


def _cmap_has_cjk(face: FontFace) -> bool:
    """独立判据：字体 cmap 里是否真的有 U+4E2D 的映射。"""
    fontTools = pytest.importorskip("fontTools.ttLib")
    try:
        font = fontTools.TTFont(str(face.path), fontNumber=face.index, lazy=True)
    except Exception:
        return False
    try:
        for table in font["cmap"].tables:
            if CJK_CODEPOINT in table.cmap:
                return True
        return False
    except Exception:
        return False
    finally:
        try:
            font.close()
        except Exception:
            pass


@pytest.fixture(scope="module")
def manager() -> FontManager:
    import tempfile

    from app.config import AppConfig, CacheCfg, LibraryCfg, RenderCfg

    tmp = Path(tempfile.mkdtemp())
    cfg = AppConfig(
        library=LibraryCfg(roots=[]),
        render=RenderCfg(fonts_dir=str(tmp / "fonts")),
        cache=CacheCfg(dir=str(tmp / "cache")),
    )
    cfg.base_dir = tmp
    return FontManager(cfg)


# --------------------------------------------------------------- 基础
@pytest.mark.parametrize(
    "ch, expected",
    [
        ("中", True),
        ("，", True),
        ("あ", True),
        ("한", True),
        ("a", False),
        ("1", False),
        (" ", False),
        ("\n", False),
    ],
)
def test_is_wide_char(ch, expected):
    assert is_wide_char(ch) is expected


def test_manager_finds_fonts(manager):
    assert manager.list_faces(), "至少应当发现一个字体（系统字体目录）"
    assert manager.default_id


def test_font_object_is_usable(manager):
    font = manager.font("auto", 40)
    assert font.getlength("中文测试") > 0


# --------------------------------------------------- 中文字形探测（核心回归）
def test_rasterize_glyph_works_at_all(manager):
    """光栅化探测本身必须能跑通。

    回归背景：旧实现用 ``font.getmask(...).tobytes()``，而 Pillow 12 的
    ``getmask()`` 返回 ``ImagingCore``（没有 ``tobytes()``），每次调用都抛
    AttributeError 并被 ``except Exception`` 吞掉 —— 探测**永远返回 False**。
    """
    face = manager.list_faces()[0]
    result = _rasterize_glyph(face, "A")
    assert result is not None, "连拉丁字母都渲染不出来，说明探测实现本身是坏的"


def test_cjk_detection_agrees_with_font_cmap(manager):
    """对含中文字形的字体，探测必须给出 True。

    这是本次修复的核心回归测试：旧实现在**所有**字体上都返回 False，
    于是 discover() 里"优先选中文"的分支全部失效，默认字体退化成 Arial，
    整本中文小说渲染成方框。
    """
    faces = manager.list_faces()
    oracle_cjk = [f for f in faces if _cmap_has_cjk(f)]
    if not oracle_cjk:
        pytest.skip("本机没有安装任何含中文字形的字体")

    detected = [f for f in oracle_cjk if manager._can_render_cjk(f)]
    assert detected, (
        f"{len(oracle_cjk)} 个字体在 cmap 里有 U+4E2D，探测却全部返回 False —— "
        "字形探测实现坏了（曾经是 Pillow API 变更导致的）"
    )


def test_default_font_can_render_chinese(manager):
    """默认字体必须画得出中文 —— 否则整个产品对中文书不可用。"""
    faces = manager.list_faces()
    if not any(_cmap_has_cjk(f) for f in faces):
        pytest.skip("本机没有安装任何含中文字形的字体")

    default = next((f for f in faces if f.id == manager.default_id), None)
    assert default is not None, f"default_id={manager.default_id} 不在字体表里"
    assert manager.has_cjk_font(), (
        f"默认字体 {default.id}（{default.path.name}）画不出中文；"
        "discover() 没有优先选择含中文字形的字体"
    )


def test_notdef_is_detected_as_missing(manager):
    """确实没有该字形的字体必须被判为 False（不能一律 True）。"""
    faces = manager.list_faces()
    lacks = [f for f in faces if not _cmap_has_cjk(f) and f.path.suffix.lower() in (".ttf", ".otf")]
    if not lacks:
        pytest.skip("本机没有可用于对照的纯拉丁字体")
    # 抽几个对照，避免太慢
    sample = lacks[:5]
    verdicts = [manager._can_render_cjk(f) for f in sample]
    assert not any(verdicts), (
        f"这些字体在 cmap 里没有 U+4E2D，却被判为可渲染中文："
        f"{[f.id for f, v in zip(sample, verdicts) if v]}"
    )


def test_detection_is_cached(manager):
    face = manager.list_faces()[0]
    first = manager._can_render_cjk(face, "中")
    second = manager._can_render_cjk(face, "中")
    assert first == second


# --------------------------------------------------------------- 名称兜底
def test_name_suggests_cjk():
    def face(name: str) -> FontFace:
        return FontFace(id=name, name=name, path=Path(f"{name}.ttf"))

    assert _name_suggests_cjk(face("msyh"))
    assert _name_suggests_cjk(face("NotoSansSC-Regular"))
    assert _name_suggests_cjk(face("SourceHanSansSC"))
    assert not _name_suggests_cjk(face("arial"))


def test_discovery_prefers_builtin_fonts(tmp_path: Path):
    """内置字体目录里的字体优先于系统字体（用户按 README 放了字体就该用它）。"""
    import shutil
    import tempfile

    from app.config import AppConfig, CacheCfg, LibraryCfg, RenderCfg

    # 从系统里借一个字体放进"内置"目录
    probe = FontManager(
        _cfg_for(tempfile.mkdtemp())
    )
    source = next(
        (f.path for f in probe.list_faces() if f.path.suffix.lower() in (".ttf", ".otf")),
        None,
    )
    if source is None:
        pytest.skip("本机没有可复制的 .ttf 字体")

    fonts_dir = tmp_path / "fonts"
    fonts_dir.mkdir()
    shutil.copy2(source, fonts_dir / source.name)

    cfg = AppConfig(
        library=LibraryCfg(roots=[]),
        render=RenderCfg(fonts_dir=str(fonts_dir), default_font=source.stem),
        cache=CacheCfg(dir=str(tmp_path / "cache")),
    )
    cfg.base_dir = tmp_path
    manager = FontManager(cfg)

    builtin = [f for f in manager.list_faces() if f.builtin]
    assert builtin, "内置目录里的字体必须被发现"
    assert manager.default_id == source.stem, (
        f"render.default_font 指定了 {source.stem}，实际选了 {manager.default_id}"
    )


def _cfg_for(tmp: str):
    from app.config import AppConfig, CacheCfg, LibraryCfg, RenderCfg

    cfg = AppConfig(
        library=LibraryCfg(roots=[]),
        render=RenderCfg(fonts_dir=str(Path(tmp) / "fonts")),
        cache=CacheCfg(dir=str(Path(tmp) / "cache")),
    )
    cfg.base_dir = Path(tmp)
    return cfg
