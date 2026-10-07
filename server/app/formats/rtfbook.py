"""RTF 适配器（M5 Phase 1）：提取纯文本 → 流式阅读。

优先用 striprtf（若安装）；否则走内置轻量解析（处理 \\'hh 字节转义、\\uN、
控制字与分组），保证零依赖可用。格式细节（字体/颜色）有意忽略 —— 与 EPUB
的"统一排版"取舍一致。
"""

from __future__ import annotations

import re
from typing import Tuple

from .base import AdapterEnv, BookRef, ParseError
from .txt import TxtAdapter


def rtf_to_text(data: bytes) -> str:
    try:
        from striprtf.striprtf import rtf_to_text as _striprtf  # noqa: PLC0415

        text = _striprtf(data.decode("latin-1", errors="replace"), errors="ignore")
        if text.strip():
            return text
    except Exception:
        pass
    return _naive_rtf(data)


# 主循环按 pos 直接匹配（不用 src[pos:] 切片：那是 O(n²)，CJK RTF 每字一个
# \uNNNN 时会慢到不可用）。match(src, pos) 的 m.end() 返回绝对偏移。
_UNICODE_AT = re.compile(r"\\u(-?\d+)\s?\??")
_CTRL_AT = re.compile(r"\\([a-zA-Z]+)(-?\d+)?[ ]?")

# 整组跳过的目的地（声明表类，内容不是正文）
_SKIP_DESTS = {
    "\\fonttbl", "\\colortbl", "\\stylesheet", "\\info", "\\pict",
    "\\themedata", "\\datastore", "\\header", "\\footer", "\\footnote",
    "\\field", "\\generator", "\\listtable", "\\rsidtbl",
}


def _naive_rtf(data: bytes) -> str:
    """轻量 RTF 文本提取：跳过忽略组/声明表，剥离控制字，解释字节与 Unicode 转义。"""
    src = data.decode("latin-1", errors="replace")
    n = len(src)
    out: list = []
    pos = 0

    def skip_group(i: int) -> int:
        """跳过从 ``i``（指向 '{'）开始的整组，返回下一位置。"""
        depth = 0
        while i < n:
            c = src[i]
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth <= 0:
                    return i + 1
            i += 1
        return i

    while pos < n:
        ch = src[pos]
        if ch == "{":
            j = pos + 1
            if src.startswith("\\*", j):
                pos = skip_group(pos)
                continue
            m = re.match(r"\\([a-zA-Z]+)", src[j : j + 32])
            if m and ("\\" + m.group(1)) in _SKIP_DESTS:
                pos = skip_group(pos)
                continue
            pos += 1
            continue
        if ch == "}":
            pos += 1
            continue
        if ch == "\\":
            if src.startswith(("\\\\", "\\{", "\\}"), pos):
                out.append(src[pos + 1])
                pos += 2
                continue
            if src.startswith("\\'", pos) and pos + 3 < n:
                try:
                    out.append(bytes([int(src[pos + 2 : pos + 4], 16)]).decode("cp1252", errors="replace"))
                except ValueError:
                    pass
                pos += 4
                continue
            m = _UNICODE_AT.match(src, pos)
            if m:
                code = int(m.group(1))
                if code < 0:
                    code += 65536
                try:
                    out.append(chr(code))
                except ValueError:
                    pass
                pos = m.end()
                continue
            m = _CTRL_AT.match(src, pos)
            if m:
                word = m.group(1)
                if word in ("par", "line", "sect", "page"):
                    out.append("\n")
                elif word == "tab":
                    out.append("\t")
                pos = m.end()
                continue
            pos += 1
            continue
        out.append(ch)
        pos += 1

    result = "".join(out)
    result = result.replace("\r", "\n")
    result = re.sub(r"\n{3,}", "\n\n", result)
    return result.strip()


class RtfAdapter(TxtAdapter):
    format = "rtf"

    def _load_text(self) -> Tuple[str, str]:
        if self._text is not None:
            return self._text, self._encoding
        max_mb = int(self.env.config.limits.max_book_mb or 0)
        if max_mb and (self.ref.size or 0) > max_mb * 1024 * 1024:
            raise ParseError(f"文件超过上限 {max_mb}MB")
        try:
            data = self.ref.path.read_bytes()
        except OSError as exc:
            raise ParseError(f"文件无法读取: {exc}") from exc
        text = rtf_to_text(data)
        self._text = text
        self._encoding = "rtf"
        return text, self._encoding
