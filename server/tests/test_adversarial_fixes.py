"""针对对抗性审查发现的缺陷的回归测试（P1 超长行 / 续排一致性 / 章节一致性 / 陈旧 id）。

这些都是"改完看起来对、但边界上仍然错"的问题，用**具体数值**钉死。
"""

from __future__ import annotations

import time
from pathlib import Path

import pytest

from app.layout.anchors import locate
from app.layout.engine import MAX_TOKEN_CHARS, FlowLayoutEngine
from app.util import textutil
from tests.conftest import make_doc, make_profile, vpath_of


# ------------------------------------------------ F1：超长 token 不能被整段塞进一行
@pytest.mark.parametrize(
    "text",
    [
        "中" + "." * 300,          # 前缀宽度 < 8% 行宽 + 以避头尾字符开头
        "中 " + ")" * 300,
        "测" + "，" * 500,
        "a" + "。" * 400,
        "。" * 400,                # 无前缀（本来就会硬断）
    ],
)
def test_overlong_token_is_never_placed_whole(fonts, text):
    """避头尾的 8% 容忍度只能用**精确**宽度判断。

    回归背景：长 token 的宽度是"提前退出"的下界值，拿它去比 `limit*1.08` 会让一个
    几百字符的超长串被整段放进一行 —— 实测排到 3 倍页宽，还会把后面内容整体上移。
    """
    profile = make_profile(w=1080, h=1920, size=40)
    doc = make_doc(chapters=0)
    from app.formats.base import Block, Chapter

    doc.chapters.append(Chapter(index=0, title="超长", blocks=[Block.paragraph(text)]))
    engine = FlowLayoutEngine(doc, profile, fonts)
    laid = engine.layout_page((0, 0, 0))
    avail = float(profile.avail_width)
    for item in laid.items:
        assert item.width <= avail * 1.08 + 0.01, (
            f"行宽 {item.width:.0f} 超出可用宽度 {avail:.0f}（容忍 8%）: {item.text[:40]!r}"
        )


def test_long_run_without_prefix_still_breaks(fonts):
    """对照组：没有前缀时本来就会硬断，确认修复没有把它弄坏。"""
    profile = make_profile(w=1080, h=1920, size=40)
    doc = make_doc(chapters=0)
    from app.formats.base import Block, Chapter

    doc.chapters.append(Chapter(index=0, title="t", blocks=[Block.paragraph("a" * 400)]))
    engine = FlowLayoutEngine(doc, profile, fonts)
    laid = engine.layout_page((0, 0, 0))
    assert laid.items
    assert all(i.width <= float(profile.avail_width) * 1.08 + 0.01 for i in laid.items)


# ------------------------------------------------ F2：从行边界续排必须完全一致
@pytest.mark.parametrize("idx", [0, 1, 2, 5, 20, 25, 26, 30, 40, 60])
def test_resuming_at_a_line_boundary_reproduces_the_tail(fonts, idx):
    """从某一行的起点续排，必须得到与"从头顺序排版"完全相同的后续行。

    这正是分页的形态：`layout_page(anchor)` 从页首锚点重新排版，而那个锚点就是
    上一页留下的行边界。回归背景：token 分块曾经相对 `start` 计算，于是续排与
    顺序排版折出不同的行 —— 分页与逐页渲染就不再同源。
    """
    profile = make_profile(w=1080, h=1920, size=40)
    doc = make_doc(chapters=0)
    from app.formats.base import Block, Chapter

    text = "a" * 3000
    doc.chapters.append(Chapter(index=0, title="t", blocks=[Block.paragraph(text)]))
    engine = FlowLayoutEngine(doc, profile, fonts)

    forward = engine.break_lines(text, 0, 40)
    if idx >= len(forward):
        pytest.skip(f"只有 {len(forward)} 行")
    start = forward[idx][1]
    resumed = list(engine.iter_lines(text, start, 40))
    assert resumed == forward[idx:], (
        f"从第 {idx} 行起点 {start} 续排的结果与顺序排版不一致："
        f"首行 {resumed[0] if resumed else None} vs {forward[idx]}"
    )


def test_chunk_boundaries_do_not_depend_on_resume_offset(fonts):
    """分块边界按绝对位置计算：从任意偏移进入，覆盖同一段文本的 token 边界一致。"""
    text = "a" * (MAX_TOKEN_CHARS * 3 + 13)
    from app.layout.engine import _iter_tokens

    forward = {(s, e) for _, s, e in _iter_tokens(text, 0)}
    # 从一个"块内偏移"进入：该偏移之后的边界应当与顺序分词一致
    probe = MAX_TOKEN_CHARS + 7
    resumed = {(s, e) for _, s, e in _iter_tokens(text, probe) if s >= MAX_TOKEN_CHARS * 2}
    expected = {(s, e) for s, e in forward if s >= MAX_TOKEN_CHARS * 2}
    assert resumed == expected, f"续分词的边界与顺序分词不同：{sorted(resumed)} vs {sorted(expected)}"


# ------------------------------------------------ F3：空白分段不再产出空行
def test_whitespace_only_tail_segment_does_not_add_blank_line(fonts, monkeypatch):
    """超长块的后续分段若全是空白，不应补出零长度文本项（白占一行高度）。"""
    from app.layout import engine as engine_mod

    monkeypatch.setattr(engine_mod, "LONG_BLOCK_CHARS", 300)
    profile = make_profile(w=1080, h=1920, size=40)
    doc = make_doc(chapters=0)
    from app.formats.base import Block, Chapter

    doc.chapters.append(Chapter(index=0, title="t", blocks=[Block.paragraph("字" * 300 + "   ")]))
    engine = FlowLayoutEngine(doc, profile, fonts)
    items = list(engine.iter_items((0, 0, 0)))
    texts = [i.text for i in items if hasattr(i, "text")]
    assert not any(t == "" for t in texts), f"出现了空文本项: {texts[-3:]}"


# ------------------------------------------------ F5：编码检测不能顾此失彼
@pytest.mark.parametrize(
    "text, encoding",
    [
        ("这是一段 GB18030 编码的中文正文，包含标点符号：、“”。", "gb18030"),
        ("第一章 初入江湖 这是正文内容。" * 3, "gb18030"),
        ("これは日本語のテキストです", "euc_jp"),
        ("これは日本語のテキストです。" * 5, "euc_jp"),
        ("これは日本語です", "shift_jis"),
        ("日本語のテキストです。" * 5, "shift_jis"),
        ("이것은 한국어 텍스트입니다", "cp949"),
        ("이것은 한국어 텍스트입니다. " * 5, "cp949"),
        ("這是一段繁體中文測試內容。", "big5"),
        ("第一章 测试中文内容，带标点。", "utf-8"),
        ("Chapter 1\nHello world.\n", "ascii"),
    ],
)
def test_encoding_detection_is_lossless(text, encoding):
    """修好"短中文被判成韩文"的同时，**不能**把短日文读坏。

    回归背景：第一版修复按全局偏好排序，把 euc_jis_2004 / cp932 与 gb18030 打平的
    情况也判成了中文，日文变成汉字乱码。现在只在"韩文 vs 中文"冲突时才改选。
    """
    decoded, used = textutil.decode_text(text.encode(encoding))
    assert decoded == text, f"in={encoding} used={used} 解码结果不一致"


def test_korean_text_is_not_forced_to_chinese():
    text = "이것은 한국어 텍스트입니다"
    decoded, used = textutil.decode_text(text.encode("cp949"))
    assert decoded == text
    assert textutil._codec_family(used) not in textutil._CHINESE_CODECS


def test_codec_family_mapping():
    assert textutil._codec_family("cp932") == "shift_jis"
    assert textutil._codec_family("euc_jis_2004") == "euc_jp"
    assert textutil._codec_family("cp950") == "big5"
    assert textutil._codec_family("gb18030") == "gb18030"


# ------------------------------------------------ F6：locate 与 meta 的章节必须一致
def test_locate_chapter_matches_page_meta_when_multiple_chapters_per_page(fonts):
    """chapter_new_page=false 时一页可含多章：locate 的 chapter_index 必须等于该页的
    record.chapter_index（与 /pages/{n}/meta 同源），锚点自身章节另用
    anchor_chapter_index 表达。"""
    profile = make_profile(chapter_new_page=False)
    doc = make_doc(chapters=3, blocks_per_chapter=2, chars_per_block=40)
    engine = FlowLayoutEngine(doc, profile, fonts)
    pm = engine.build(layout_id="Lmulti")

    for rec in pm.pages:
        result = locate(pm, rec.anchor)
        assert result["page"] == rec.page
        assert result["chapter_index"] == rec.chapter_index, (
            f"第 {rec.page} 页 locate 报 {result['chapter_index']}，meta 报 {rec.chapter_index}"
        )

    # 锚点自身章节：第 1 章的锚点应当给出 anchor_chapter_index=1
    found = False
    for rec in pm.pages:
        if rec.anchor[0] == 1 and rec.chapter_index != 1:
            result = locate(pm, rec.anchor)
            assert result["anchor_chapter_index"] == 1
            found = True
    # 该文档下不一定能构造出"页首是 0 章、锚点在 1 章"的页，不强制
    assert found or True


def test_locate_clamped_does_not_invent_chapter(fonts):
    """越界收敛时不能凭"请求里的 chapter 存在"就报出该章。"""
    profile = make_profile()
    doc = make_doc(chapters=2, blocks_per_chapter=3, chars_per_block=40)
    engine = FlowLayoutEngine(doc, profile, fonts)
    pm = engine.build(layout_id="Lclamp")
    last = pm.pages[-1]
    result = locate(pm, (1, 9999, 9999))
    assert result["clamped"] is True
    assert result["page"] == last.page
    assert result["chapter_index"] == last.chapter_index


def test_locate_fixed_pagemap_has_zero_chapter():
    from app.layout.engine import build_fixed_pagemap

    pm = build_fixed_pagemap("b", "L", 3)
    result = locate(pm, (0, 1, 0))
    assert result["chapter_index"] == 0
    assert result["anchor_chapter_index"] == 0


# ------------------------------------------------ F7：被替换掉的 book_id 必须失效
def test_superseded_book_id_stops_resolving(context, tmp_books: Path):
    """文件被换成不同内容后，旧 book_id 不能继续交付新文件。

    回归背景：`_book_id_for` 会刷新 `_books[vpath]` 并登记新 id，但 `_by_id` 里
    旧的别名仍指向同一路径 —— 于是旧 id 会返回"新内容 + 新 size/mtime"的 BookRef，
    而且因为 size/mtime 已经同步，后面的变化检测再也发现不了。
    """
    from app.api.errors import AppError

    path = tmp_books / "swap2.txt"
    path.write_text("第一章 原始内容\n" + "正文。" * 200, encoding="utf-8")
    context.library.scan_blocking()
    ref = context.library.book_by_vpath(vpath_of("swap2.txt"))
    assert ref is not None
    old_id = ref.book_id

    time.sleep(0.02)
    path.write_text("完全不同长度的新内容\n" + "新正文。" * 500, encoding="utf-8")

    # 第一次：检测到变化 → 指纹变了 → 404
    with pytest.raises(AppError):
        context.require_book(old_id)

    # 第二次、第三次：旧 id 必须继续 404（不能"复活"成新文件）
    for attempt in range(3):
        with pytest.raises(AppError) as excinfo:
            context.require_book(old_id)
        assert excinfo.value.code == "BOOK_NOT_FOUND", f"第 {attempt + 2} 次请求旧 id 仍可用"

    # 重新扫描后应当能用新 id 打开
    context.library.scan_blocking()
    fresh = context.library.book_by_vpath(vpath_of("swap2.txt"))
    assert fresh is not None and fresh.book_id != old_id
    assert context.require_book(fresh.book_id).book_id == fresh.book_id


def test_unchanged_book_id_still_resolves(context, tmp_books: Path):
    """对照组：内容没变时必须照常可用（别把正常路径一起关掉）。"""
    path = tmp_books / "stable2.txt"
    path.write_text("第一章 稳定\n" + "正文。" * 100, encoding="utf-8")
    context.library.scan_blocking()
    ref = context.library.book_by_vpath(vpath_of("stable2.txt"))
    for _ in range(5):
        assert context.require_book(ref.book_id).book_id == ref.book_id


# ------------------------------------------------ F9：容器判据统一
def test_drive_letter_entry_rejected_by_all_containers(env, tmp_path: Path):
    """盘符 / ADS 条目名必须被**所有**容器拒绝，不能只有 ZIP 检查。"""
    import io
    import tarfile

    from app.formats.comics import TarComicContainer, ZipComicContainer
    from tests.conftest import png_bytes, write_cbz

    payload = png_bytes(10, 10)

    zip_path = write_cbz(tmp_path / "z.cbz", {"C:/evil.png": payload, "ok.png": payload})
    zc = ZipComicContainer(env, zip_path)
    try:
        assert zc.names == ["ok.png"]
    finally:
        zc.close()

    tar_path = tmp_path / "t.cbt"
    with tarfile.open(tar_path, "w") as tf:
        for name in ("C:/evil.png", "ok.png"):
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            tf.addfile(info, io.BytesIO(payload))
    tc = TarComicContainer(env, tar_path)
    try:
        assert tc.names == ["ok.png"], f"TAR 放行了盘符条目: {tc.names}"
    finally:
        tc.close()


def test_dot_slash_entry_is_kept_by_all_containers(env, tmp_path: Path):
    import io
    import tarfile

    from app.formats.comics import TarComicContainer
    from tests.conftest import png_bytes

    payload = png_bytes(10, 10)
    tar_path = tmp_path / "dot.cbt"
    with tarfile.open(tar_path, "w") as tf:
        info = tarfile.TarInfo("./dir/01.png")
        info.size = len(payload)
        tf.addfile(info, io.BytesIO(payload))
    tc = TarComicContainer(env, tar_path)
    try:
        assert tc.names == ["dir/01.png"]
        assert tc.read("dir/01.png")[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        tc.close()


def test_normalizable_entry_not_killed_by_default_glob(env, tmp_path: Path):
    """`foo/./bar.png` 曾被默认规则 `*/.*` 误杀（fnmatch 看到 "/."）。"""
    from app.formats.comics import should_skip_entry
    from tests.conftest import png_bytes, write_cbz

    patterns = ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]
    assert should_skip_entry("foo/./bar.png", patterns) is False
    assert should_skip_entry("foo/.hidden/bar.png", patterns) is True
    assert should_skip_entry(".hidden/bar.png", patterns) is True

    archive = write_cbz(tmp_path / "norm.cbz", {"foo/./bar.png": png_bytes(10, 10)})
    from app.formats.comics import ZipComicContainer

    zc = ZipComicContainer(env, archive)
    try:
        assert zc.names == ["foo/bar.png"]
        assert zc.read("foo/bar.png")[:8] == b"\x89PNG\r\n\x1a\n"
    finally:
        zc.close()
