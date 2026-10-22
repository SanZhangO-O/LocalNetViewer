"""目录浏览与书库搜索端点（设计 5.2 #2 / #5.3）。"""

from __future__ import annotations

import asyncio
from typing import Any

from fastapi import APIRouter, Depends, Query

from ..models.schemas import (
    FileEntry,
    FilesResponse,
    GalleryInfo,
    LibraryInfo,
    LibrarySearchResponse,
)
from ..services.access import AccessScope
from .deps import ScopeDep, get_ctx

router = APIRouter(tags=["files"])

MAX_LIMIT = 5000


def _to_entry(item: dict) -> FileEntry:
    """服务端条目 dict → 响应模型（目录列表与书库搜索共用，字段只维护一份）。"""
    return FileEntry(
        name=str(item.get("name", "")),
        type=str(item.get("type", "file")),  # type: ignore[arg-type]
        path=str(item.get("path", "")),
        format=item.get("format"),
        book_id=item.get("book_id"),
        size=item.get("size"),
        mtime=item.get("mtime"),
        cover_url=item.get("cover_url"),
        gallery=GalleryInfo(**item["gallery"]) if item.get("gallery") else None,
        openable=bool(item.get("openable", True)),
        reason=item.get("reason"),
        has_contents=bool(item.get("has_contents", False)),
        media_kind=item.get("media_kind"),
        stream_url=item.get("stream_url"),
        mime=item.get("mime"),
    )


@router.get("/files", response_model=FilesResponse)
async def list_files(
    path: str = Query(default="/", description="虚拟路径，'/' 列出所有书库根"),
    limit: int = Query(default=1000, ge=1, le=MAX_LIMIT),
    offset: int = Query(default=0, ge=0),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> FilesResponse:
    data = await asyncio.to_thread(ctx.library.list_dir, path, limit, offset, scope)
    # 索引落盘是同步 IO（可能几 MB），不能放在事件循环里
    await asyncio.to_thread(ctx.library.maybe_save)
    entries = [_to_entry(item) for item in data.get("entries", [])]
    libraries = None
    if data.get("libraries"):
        libraries = [
            LibraryInfo(
                name=str(lib.get("name", "")),
                book_count=int(lib.get("book_count", 0)),
                # 真实文件系统路径不下发（设计 9；与 /server/info 一致）
                path=None,
            )
            for lib in data["libraries"]
        ]
    return FilesResponse(
        path=str(data.get("path", "/")),
        parent=data.get("parent"),
        gallery=GalleryInfo(**data["gallery"]) if data.get("gallery") else None,
        entries=entries,
        total=int(data.get("total", len(entries))),
        limit=int(data.get("limit", limit)),
        offset=int(data.get("offset", offset)),
        scanning=bool(data.get("scanning", False)),
        libraries=libraries,
    )


@router.get("/library/search", response_model=LibrarySearchResponse)
async def search_library(
    q: str = Query(min_length=1, max_length=120, description="书名 / 路径关键词，空格分词"),
    limit: int = Query(default=50, ge=1, le=200),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> LibrarySearchResponse:
    """书库搜索：只匹配内存索引里的虚拟路径，不 stat、不读文件（设计 5.2 #17）。"""
    data: dict[str, Any] = await asyncio.to_thread(
        ctx.library.search_books, q, limit, scope
    )
    return LibrarySearchResponse(
        query=q,
        entries=[_to_entry(item) for item in data.get("entries", [])],
        total=int(data.get("total", 0)),
        truncated=bool(data.get("truncated", False)),
        scanning=ctx.library.scanning,
    )
