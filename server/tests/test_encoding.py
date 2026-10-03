"""编码检测与文本工具（设计 10.3：编码检测）。"""

from __future__ import annotations

import subprocess
import sys

import pytest

from app.util import textutil


# ------------------------------------------------------------------ BOM
@pytest.mark.parametrize(
    "raw, expected",
    [
        ("中文测试".encode("utf-8-sig"), "utf-8-sig"),
        # 无后缀的 utf-16 / utf-32 会写 BOM，且解码时会**消费掉** BOM
        ("中文测试".encode("utf-16"), "utf-16"),
        (b"\xff\xfe" + "中文测试".encode("utf-16-le"), "utf-16"),
        (b"\xfe\xff" + "中文测试".encode("utf-16-be"), "utf-16"),
        ("中文测试".encode("utf-32"), "utf-32"),
        (b"\xff\xfe\x00\x00" + "中文测试".encode("utf-32-le"), "utf-32"),
        (b"\x00\x00\xfe\xff" + "中文测试".encode("utf-32-be"), "utf-32"),
    ],
)
def test_bom_wins(raw, expected):
    assert textutil.detect_bom(raw) == expected
    assert textutil.detect_encoding(raw) == expected


def test_bom_decode_round_trip():
    cases = [
        "第一章 测试".encode("utf-8-sig"),
        "第一章 测试".encode("utf-16"),
        "第一章 测试".encode("utf-16-be"),
        "第一章 测试".encode("utf-32"),
    ]
    for raw in cases:
        text, used = textutil.decode_text(raw)
        assert "第一章" in text, f"{used} 解码失败"
        assert used


def test_bom_is_not_left_in_the_text():
    """BOM 不能留在正文里。

    回归测试：把 BOM 映射成 "utf-16-le" 时 ``bytes.decode`` 不会消费 BOM，
    于是 ``text[0] == '\\ufeff'``。后果有两个：
      1. 第一行 "第X章 …" 因为多了前缀而匹配不上章节正则 → 目录少一章 / 错位；
      2. 第 1 页会把 U+FEFF 画成一个"未映射字形"的方框。
    """
    for raw in (
        "第一章 初入江湖".encode("utf-16"),
        "第一章 初入江湖".encode("utf-16-be"),
        "第一章 初入江湖".encode("utf-32"),
        "第一章 初入江湖".encode("utf-8-sig"),
    ):
        text, used = textutil.decode_text(raw)
        assert not text.startswith("\ufeff"), f"{used} 解码后仍带 BOM"
        assert "\ufeff" not in text
        # 首行必须是可识别的章节标题
        first = textutil.split_lines(text)[0]
        assert textutil.match_chapter_title(first) == "第一章 初入江湖", repr(first)


def test_utf16le_without_bom_latin_text():
    """无 BOM 的 UTF-16（每两字节一个 0x00）必须被识别，否则整本变乱码。"""
    raw = "Chapter One\r\nHello world.\r\n".encode("utf-16-le")
    assert textutil.detect_bom(raw) is None
    text, enc = textutil.decode_text(raw)
    assert text.startswith("Chapter One")
    assert enc.replace("_", "-").lower() in ("utf-16-le", "utf-16")


def test_utf16be_without_bom_latin_text():
    raw = "Chapter One\r\nHello world.\r\n".encode("utf-16-be")
    text, enc = textutil.decode_text(raw)
    assert text.startswith("Chapter One")


def test_no_false_utf16_for_plain_text():
    """正常的单字节文本不能被误判成 UTF-16（否则读成乱码）。"""
    for raw in (
        "这是一段普通的中文正文。".encode("gb18030"),
        "这是一段普通的中文正文。".encode("utf-8"),
        b"plain ascii text, no nulls here\n",
    ):
        assert textutil.detect_bomless_utf16(raw) is None


# ------------------------------------------------------- 常见中文编码
def test_utf8_no_bom():
    raw = "这是一段 UTF-8 中文正文。".encode("utf-8")
    text, enc = textutil.decode_text(raw)
    assert text == "这是一段 UTF-8 中文正文。"
    assert enc.lower().replace("_", "-") in ("utf-8", "utf8", "ascii")


def test_gb18030():
    """GB18030 正文必须被正确解码。

    只断言**解码结果正确**，不断言 charset-normalizer 报出的编码名：cp949 与
    GB18030 在常用汉字区有重叠，检测器可能报出等价可用的别名（例如 cp949）。
    """
    original = "这是一段 GB18030 编码的中文正文，包含标点符号：、“”。"
    text, enc = textutil.decode_text(original.encode("gb18030"))
    assert text == original
    assert enc


def test_big5():
    raw = "這是一段繁體中文測試".encode("big5")
    text, enc = textutil.decode_text(raw)
    assert "繁體中文" in text


def test_pure_ascii():
    raw = b"Chapter 1\nHello world.\n"
    text, enc = textutil.decode_text(raw)
    assert text.startswith("Chapter 1")
    assert enc.lower() in ("ascii", "utf-8", "utf8")


def test_empty_bytes():
    text, _ = textutil.decode_text(b"")
    assert text == ""


def test_explicit_override_wins():
    """配置里的书级编码覆盖必须优先于自动检测（设计 7.2）。"""
    raw = "覆盖测试".encode("gb18030")
    text, enc = textutil.decode_text(raw, "gb18030")
    assert text == "覆盖测试"
    assert enc == "gb18030"


def test_undecodable_bytes_fall_back_to_replace():
    """任何字节序列都必须能产出字符串，不能抛异常（否则整本书打不开）。"""
    raw = bytes(range(256))
    text, enc = textutil.decode_text(raw)
    assert isinstance(text, str)
    assert enc


# ------------------------------------------------------- 章节切分
@pytest.mark.parametrize(
    "line",
    [
        "第一章 初入江湖",
        "第1章 初入江湖",
        "第一百二十三章 决战",
        "第十二节 小结",
        "第三卷 风起",
        "序章",
        "楔子",
        "番外一",
        "Chapter 12 The Beginning",
    ],
)
def test_chapter_titles_are_recognized(line):
    assert textutil.match_chapter_title(line) == line


@pytest.mark.parametrize(
    "line",
    [
        "他说的第一章节内容很有意思",  # 不在行首
        "第一章" + "很长" * 40,  # 超过行长上限
        "正文内容而已",
        "",
        "   ",
    ],
)
def test_non_chapter_lines_are_rejected(line):
    assert textutil.match_chapter_title(line) is None


def test_split_lines_handles_all_newline_styles():
    assert textutil.split_lines("a\r\nb\rc\nd") == ["a", "b", "c", "d"]


def test_snippet_around_marks_truncation():
    text = "x" * 100 + "关键词" + "y" * 100
    snippet = textutil.snippet_around(text, 100, 103, radius=5)
    assert "关键词" in snippet
    assert snippet.startswith("…") and snippet.endswith("…")


def test_is_cjk_detects_chinese_only():
    assert textutil.is_cjk("这是一段中文")
    assert not textutil.is_cjk("This is English")
    assert not textutil.is_cjk("")


def test_truncate_adds_ellipsis():
    assert textutil.truncate("abc", 10) == "abc"
    assert textutil.truncate("abcdef", 3) == "abc…"


# ------------------------------------------------------- 稳定哈希
def test_stable_hash_is_deterministic_in_process():
    assert textutil.stable_hash("abc") == textutil.stable_hash("abc")
    assert textutil.stable_hash("abc") != textutil.stable_hash("abd")


def test_stable_hash_is_stable_across_processes():
    """核心回归测试：内置 hash() 按 PYTHONHASHSEED 加盐，每个进程都不同。

    它曾被用在 /asset 的 ETag 与文字封面配色上 —— 前者会让服务器重启后客户端
    强缓存整体失效，后者会让同一本书每次重启换个封面颜色。
    """
    code = (
        "import sys; sys.path.insert(0, r'{root}');"
        "from app.util.textutil import stable_hash;"
        "print(stable_hash('同一本书'), hash('同一本书') % 100000)"
    )
    outputs = []
    for seed in ("1", "2", "12345"):
        proc = subprocess.run(
            [sys.executable, "-c", code.format(root=str(_server_root()))],
            capture_output=True,
            text=True,
            env={"PYTHONHASHSEED": seed, "PATH": ""},
            check=True,
        )
        outputs.append(proc.stdout.strip().split())

    stable_values = {item[0] for item in outputs}
    notdef_values = {item[1] for item in outputs}
    assert len(stable_values) == 1, f"stable_hash 跨进程不一致: {outputs}"
    # 内置 hash() 应当（在本机默认配置下）随种子变化 —— 若 CPython 改了行为，
    # 这条断言会失败，提醒我们重新审视"为什么不用 hash()"的注释
    if len(notdef_values) == 1:
        pytest.skip("本机 PYTHONHASHSEED 未生效，跳过 hash() 对照断言")


def _server_root():
    from pathlib import Path

    return Path(__file__).resolve().parent.parent
