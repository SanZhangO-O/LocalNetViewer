"""HTTP Range 头解析（单一实现，pages 与 media 共用）。"""

from __future__ import annotations

from typing import Optional, Tuple


def parse_range_header(value: Optional[str], total: int) -> Tuple[str, int, int]:
    """解析单段 Range：返回 ``(kind, start, end)``，kind ∈ none | ok | unsatisfiable。

    语义（RFC 9110，与既有 ``routes_read._slice_range`` 保持一致）：
    - 语法无效（含多段、缺 ``-``）→ ``none``：调用方忽略并按 200 返回整段；
    - 语法有效但超出范围 → ``unsatisfiable``：调用方 416 + ``Content-Range: bytes */total``。
    """
    if not value or not value.lower().startswith("bytes="):
        return "none", 0, 0
    spec = value.split("=", 1)[1].split(",")[0].strip()
    if "-" not in spec:
        return "none", 0, 0
    start_s, _, end_s = spec.partition("-")
    try:
        if start_s == "":
            if end_s == "":
                return "none", 0, 0
            length = int(end_s)
            start = max(0, total - length)
            end = total - 1
        else:
            start = int(start_s)
            end = int(end_s) if end_s else total - 1
    except ValueError:
        return "none", 0, 0
    if start < 0 or end < 0:
        return "none", 0, 0
    if start >= total:
        return "unsatisfiable", 0, 0
    end = min(end, total - 1)
    if end < start:
        return "unsatisfiable", 0, 0
    return "ok", start, end
