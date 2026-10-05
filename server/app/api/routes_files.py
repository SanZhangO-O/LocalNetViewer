"""目录浏览端点（设计 5.2 #2 / 5.3）。"""

from __future__ import annotations

import asyncio

from fastapi import APIRouter, Depends, Query

from ..models.schemas import FileEntry, FilesResponse, GalleryInfo, LibraryInfo
from ..services.access import AccessScope
from .deps import ScopeDep, get_ctx

router = APIRouter(tags=["files"])

MAX_LIMIT = 5000


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
    entries = [
        FileEntry(
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
        )
        for item in data.get("entries", [])
    ]
    libraries = None
    if data.get("libraries"):
        libraries = [
            LibraryInfo(
                name=str(lib.get("name", "")),
                book_count=int(lib.get("book_count", 0)),
                path=lib.get("path"),
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
