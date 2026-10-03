"""全文搜索（设计 5.2 #9，P3）：TXT / EPUB 等文本类格式。

- 搜索在解析结果（DocumentModel）上进行，返回 ``chapter/block/char`` 锚点；
- 带 ``layout_id`` 时把锚点换算成页码，客户端可直接跳页；
- 大小写策略：纯 ASCII 查询词忽略大小写（偏移量不变），其它语言精确匹配。
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

from ..formats.base import DocumentModel
from ..layout.anchors import page_of_anchor
from ..layout.engine import PageMap
from ..util.textutil import snippet_around

MAX_SCAN_HITS = 500


class SearchService:
    def search(
        self,
        doc: DocumentModel,
        query: str,
        *,
        limit: int = 50,
        pagemap: Optional[PageMap] = None,
    ) -> Dict[str, Any]:
        q = (query or "").strip()
        results: List[Dict[str, Any]] = []
        if not q:
            return {"query": q, "total": 0, "results": [], "limited": False}

        limit = max(1, min(500, int(limit or 50)))
        case_insensitive = q.isascii()
        needle = q.lower() if case_insensitive else q
        total = 0
        truncated = False

        for chapter in doc.chapters:
            for block_index, block in enumerate(chapter.blocks):
                if not block.is_text:
                    continue
                text = block.text
                if not text:
                    continue
                haystack = text
                if case_insensitive:
                    lowered = text.lower()
                    # 少数特殊字符 lower() 后长度会变（如 'İ'），那会破坏 char 偏移，退回大小写敏感
                    if len(lowered) == len(text):
                        haystack = lowered
                start = 0
                while True:
                    idx = haystack.find(needle, start)
                    if idx < 0:
                        break
                    total += 1
                    if len(results) < limit:
                        page = None
                        if pagemap is not None and pagemap.pages:
                            page, _ = page_of_anchor(
                                pagemap, (chapter.index, block_index, idx)
                            )
                        results.append(
                            {
                                "chapter_index": chapter.index,
                                "chapter_title": chapter.title,
                                "block": block_index,
                                "char": idx,
                                "page": page,
                                "snippet": snippet_around(text, idx, idx + len(needle)),
                            }
                        )
                    start = idx + max(1, len(needle))
                    if total >= MAX_SCAN_HITS:
                        truncated = True
                        break
                if truncated:
                    break
            if truncated:
                break

        return {
            "query": q,
            "total": total,
            "results": results,
            "limited": truncated or total > len(results),
        }
