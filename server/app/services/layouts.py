"""排版注册服务：LayoutProfile → layout_id + PageMap（设计 3.3 / 4.6 / 4.9）。

- flow：解析（缓存命中则跳过）→ 分页（内存/磁盘缓存）；
- fixed / strip：每次 ``/layout`` 实时枚举页集合（不缓存页内容，设计 4.6）；
- 同一 ``(book_id, layout_id)`` 首次分页加锁，避免并发重复计算。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from ..cache.parsed import PARSER_VERSION, ParsedCache
from ..cache.store import LayoutEntry, LayoutStore
from ..formats.base import BookAdapter, BookRef
from ..layout.engine import (
    FlowLayoutEngine,
    build_fixed_pagemap,
    build_strip_pagemap,
)
from ..layout.fonts import FontManager
from ..models.schemas import LayoutProfile
from ..util.concurrency import KeyedLocks, run_with_timeout

# 分页算法（折行/切页）变更时递增，使旧 PageMap 自然失效
LAYOUT_ENGINE_VERSION = "1"


@dataclass
class LayoutResult:
    layout_id: str
    mode: str
    total_pages: int
    chapters: List[Dict[str, Any]] = field(default_factory=list)
    cache_hit: bool = False
    strip: bool = False


def layout_key(fonts: FontManager, profile: LayoutProfile) -> str:
    """layout_id = 几何 + 解析器版本 + 分页引擎版本 + **已解析的字体 id** 的哈希。

    设计 4.6 只说"几何相关字段"，但只哈希几何会留下两个陷阱：
    1. 解析器升级后块索引可能变化，旧 PageMap 的锚点会指向错误位置（渲染成空白页）；
    2. 客户端默认 ``family="auto"``，字体目录变化会改变实际字宽，旧分页结果随之失真。
    所以把这两项一并纳入哈希：变化即产生新的 layout_id，旧缓存自然失效。
    """
    payload = json.dumps(
        {
            "geometry": profile.geometry_dict(),
            "parser": PARSER_VERSION,
            "engine": LAYOUT_ENGINE_VERSION,
            "font": fonts.resolve_id(profile.font.family),
        },
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    )
    return "L" + hashlib.sha1(payload.encode("utf-8")).hexdigest()[:12]


def effective_profile(config, adapter: BookAdapter, profile: LayoutProfile) -> LayoutProfile:
    """按适配器能力修正 mode（固定版式不可能做文本重排；条漫仅漫画支持）。"""
    update: Dict[str, Any] = {}
    if adapter.mode == "flow":
        if profile.mode != "flow":
            update["mode"] = "flow"
    else:
        if profile.mode == "strip" and not adapter.supports_strip:
            update["mode"] = "fixed"
        elif profile.mode == "flow":
            update["mode"] = "fixed"
        if (update.get("mode", profile.mode) == "strip") and not profile.strip_overlap_px:
            cfg_overlap = int(getattr(config.comic, "strip_overlap_px", 0) or 0)
            if cfg_overlap:
                update["strip_overlap_px"] = cfg_overlap
    if not update:
        return profile
    return profile.model_copy(update=update)


class LayoutService:
    def __init__(
        self,
        config,
        store: LayoutStore,
        parsed: ParsedCache,
        fonts: FontManager,
    ) -> None:
        self.config = config
        self.store = store
        self.parsed = parsed
        self.fonts = fonts
        self._locks = KeyedLocks()

    # ------------------------------------------------------------ 查询
    def entry(self, book_id: str, layout_id: str) -> Optional[LayoutEntry]:
        if not layout_id:
            return None
        return self.store.get(book_id, layout_id)

    def invalidate(self, book_id: str) -> int:
        return self.store.invalidate_book(book_id)

    # ------------------------------------------------------------ 构建
    def build(
        self,
        ref: BookRef,
        adapter: BookAdapter,
        profile: LayoutProfile,
        *,
        timeout_s: Optional[float] = None,
    ) -> LayoutResult:
        profile = effective_profile(self.config, adapter, profile)
        layout_id = layout_key(self.fonts, profile)
        timeout = float(timeout_s or self.config.limits.parse_timeout_s)

        if profile.mode == "flow":
            entry = self.store.get(ref.book_id, layout_id)
            if entry is not None:
                return self._result(entry, cache_hit=True)
            with self._locks.acquire((ref.book_id, layout_id)):
                entry = self.store.get(ref.book_id, layout_id)
                if entry is not None:
                    return self._result(entry, cache_hit=True)
                doc = self.parsed.get_or_parse(
                    ref.book_id,
                    adapter.document,
                    timeout_s=timeout,
                    what="解析分页",
                )
                engine = FlowLayoutEngine(doc, profile, self.fonts)
                # 分页本身也必须受超时保护：解析超时只覆盖 adapter.document()，
                # 而"整本没有换行的 TXT"会让分页本身跑几分钟
                pagemap = run_with_timeout(
                    lambda: engine.build(layout_id=layout_id),
                    timeout,
                    "分页",
                    key=("layout", ref.book_id, layout_id),
                )
                entry = LayoutEntry(
                    book_id=ref.book_id,
                    layout_id=layout_id,
                    profile=profile,
                    pagemap=pagemap,
                )
                self.store.save(entry)
                return self._result(entry, cache_hit=False)

        # 固定版式 / 条漫：页集合每次实时枚举（不缓存页内容，设计 4.6）。
        # 但枚举本身可能是重活（strip 要探测每张图的原始尺寸，PDF 要开文档），
        # 与 flow 分页同等对待：同一 (book_id, layout_id) 加锁避免并发重复枚举 +
        # run_with_timeout 超时保护（超时同样映射 PARSE_FAILED；若上一次枚举
        # 超时后仍在跑，key 去重会让本次快速失败而不是再跑一遍）。
        with self._locks.acquire((ref.book_id, layout_id)):
            if profile.mode == "strip":
                slices = run_with_timeout(
                    lambda: adapter.strip_pages(
                        profile.avail_height, int(profile.strip_overlap_px)
                    ),
                    timeout,
                    "分页",
                    key=("layout", ref.book_id, layout_id),
                )
                pagemap = build_strip_pagemap(ref.book_id, layout_id, slices)
            else:
                page_count = run_with_timeout(
                    adapter.page_count,
                    timeout,
                    "分页",
                    key=("layout", ref.book_id, layout_id),
                )
                pagemap = build_fixed_pagemap(
                    ref.book_id, layout_id, page_count, mode="fixed"
                )
            entry = LayoutEntry(
                book_id=ref.book_id, layout_id=layout_id, profile=profile, pagemap=pagemap
            )
            self.store.save(entry)
        return self._result(entry, cache_hit=False)

    @staticmethod
    def _result(entry: LayoutEntry, *, cache_hit: bool) -> LayoutResult:
        return LayoutResult(
            layout_id=entry.layout_id,
            mode=entry.pagemap.mode,
            total_pages=entry.pagemap.total_pages,
            chapters=list(entry.pagemap.chapters),
            cache_hit=cache_hit,
            strip=entry.pagemap.mode == "strip",
        )

    # ------------------------------------------------------------ 解析结果
    def document(self, ref: BookRef, adapter: BookAdapter) -> Any:
        return self.parsed.get_or_parse(
            ref.book_id,
            adapter.document,
            timeout_s=float(self.config.limits.parse_timeout_s),
            what="解析",
        )
