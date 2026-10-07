"""FBZ / .fb2.zip 适配器（M5 Phase 1）：zip 包内首个 FB2 → 常规 FB2 解析。

复用压缩包容器与解压缓存（与嵌套书同一套机制），解压产物落在
``cache/extracted/<book_id>/``。
"""

from __future__ import annotations

from pathlib import Path

from ..util.paths import ext_of
from .archivebook import extract_entry_to_cache
from .base import AdapterEnv, BookAdapter, BookRef, ParseError
from .comics import open_container
from .fb2 import Fb2Adapter


def open_fbz_adapter(env: AdapterEnv, ref: BookRef) -> BookAdapter:
    container = open_container(env, Path(ref.path))
    try:
        entries = container.browse()
        target = next(
            (
                e
                for e in entries
                if not e.is_dir and ext_of(e.name) == ".fb2"
            ),
            None,
        )
        if target is None:
            raise ParseError("FBZ 内没有 .fb2 文件")
        extracted = extract_entry_to_cache(env, container, target.name, ref.book_id)
    finally:
        container.close()
    try:
        size = int(extracted.stat().st_size)
        mtime = float(extracted.stat().st_mtime)
    except OSError:
        size, mtime = 0, 0.0
    synth = BookRef(
        book_id=ref.book_id,
        vpath=ref.vpath,
        path=extracted,
        format="fb2",
        size=size,
        mtime=mtime,
        virtual=ref.virtual,
        revision=ref.revision,
        extra=dict(ref.extra or {}),
    )
    return Fb2Adapter(env, synth)
