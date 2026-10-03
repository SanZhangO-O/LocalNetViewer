"""数字感知的自然排序（设计 4.8 / 7.4：``2.jpg < 10.jpg``，忽略大小写）。

用于目录列表与漫画页序，保证两处结果完全一致。
"""

from __future__ import annotations

import re
from typing import Iterable, List, Sequence, TypeVar

_NUM_RE = re.compile(r"(\d+)")

T = TypeVar("T")


def natural_key(name: str) -> tuple:
    """把名字切成 文本/数字 交替的键，数字按数值比较。"""
    s = str(name)
    parts = _NUM_RE.split(s)
    key: List[tuple] = []
    for i, part in enumerate(parts):
        if part == "":
            continue
        if i % 2 == 1:  # 数字段
            try:
                key.append((1, int(part), ""))
            except ValueError:  # pragma: no cover
                key.append((0, 0, part.casefold()))
        else:
            key.append((0, 0, part.casefold()))
    return tuple(key)


def natural_sort(names: Iterable[str]) -> List[str]:
    return sorted(names, key=natural_key)


def natural_sort_paths(paths: Iterable[str]) -> List[str]:
    """按路径逐段自然排序（用于文件夹图集递归枚举）。"""
    return sorted(paths, key=lambda p: tuple(natural_key(seg) for seg in str(p).split("/")))


def sort_by_natural(items: Sequence[T], key=lambda x: x) -> List[T]:  # type: ignore[valid-type]
    return sorted(items, key=lambda it: natural_key(str(key(it))))
