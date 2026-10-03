"""书籍元数据、封面、书内资源与文件夹图集端点（设计 5.2 #3 / #4 / #10 / #13）。"""

from __future__ import annotations

import asyncio

from fastapi import APIRouter, Depends, Query, Response

from ..models.schemas import (
    BookMetaResponse,
    ChapterInfo,
    ErrorCode,
    OpenFolderRequest,
    OpenFolderResponse,
    RenderSupport,
)
from ..render.raster import text_cover
from ..util.paths import safe_asset_path
from ..util.textutil import stable_hash
from .deps import AuthDep, get_ctx
from .errors import AppError

router = APIRouter(tags=["books"])

COVER_MAX_AGE = 86400
IMMUTABLE = "public, max-age=31536000, immutable"


@router.get("/books/{book_id}/meta", response_model=BookMetaResponse, dependencies=[AuthDep])
async def book_meta(book_id: str, ctx=Depends(get_ctx)) -> BookMetaResponse:
    """元数据 + 章节目录 + 封面 URL（EPUB 只做容器级解析，设计 7.3）。"""
    ref = await ctx.require_book_async(book_id)
    async with ctx.open_adapter_async(ref) as adapter:
        meta = await asyncio.to_thread(adapter.meta)
    chapters = [
        ChapterInfo(
            index=int(ch.get("index", i)),
            title=str(ch.get("title") or f"第 {i + 1} 节"),
            total_chars=ch.get("total_chars"),
            start_page=ch.get("start_page"),
        )
        for i, ch in enumerate(meta.chapters or [])
    ]
    return BookMetaResponse(
        book_id=ref.book_id,
        title=meta.title or ref.title_guess,
        author=meta.author,
        format=meta.format or ref.format,
        language=meta.language,
        size=int(meta.size or ref.size or 0),
        mode=meta.mode,  # type: ignore[arg-type]
        page_count=meta.page_count,
        cover_url=f"/api/v1/books/{ref.book_id}/cover",
        render_support=RenderSupport(png=True, json=bool(meta.supports_json)),
        chapters=chapters,
        source=ref.vpath,
    )


@router.get("/books/{book_id}/cover", dependencies=[AuthDep])
async def book_cover(
    book_id: str,
    w: int = Query(default=240, ge=40, le=1024),
    ctx=Depends(get_ctx),
) -> Response:
    """封面图（JPEG/PNG，强缓存）。无内嵌封面时用 Pillow 生成标题封面。"""
    key = ("cover", book_id, int(w))
    cached = ctx.cover_cache.get(key)
    headers = {
        "Cache-Control": f"public, max-age={COVER_MAX_AGE}",
        "ETag": f'"{book_id}:w{int(w)}"',
    }
    if cached is not None:
        body, mime = cached
        return Response(content=body, media_type=mime, headers=headers)

    ref = await ctx.require_book_async(book_id)
    async with ctx.open_adapter_async(ref) as adapter:
        result = await asyncio.to_thread(adapter.cover, int(w))
        if result is None:
            meta = await asyncio.to_thread(adapter.meta)
            # 生成文字封面是 Pillow 绘制 + 编码，同样要放到线程池
            result = await asyncio.to_thread(
                text_cover,
                meta.title or ref.title_guess,
                meta.author,
                width=int(w),
                fonts=ctx.fonts,
            )
    body, mime = result
    ctx.cover_cache.put(key, (body, mime), len(body))
    return Response(content=body, media_type=mime, headers=headers)


@router.get("/books/{book_id}/asset", dependencies=[AuthDep])
async def book_asset(
    book_id: str,
    path: str = Query(..., description="容器内资源路径（EPUB 内嵌图片等）"),
    ctx=Depends(get_ctx),
) -> Response:
    """书内资源（只允许取书籍容器内部资源，做规范化校验，设计 9）。"""
    safe = safe_asset_path(path)
    ref = await ctx.require_book_async(book_id)
    async with ctx.open_adapter_async(ref) as adapter:
        data, mime = await asyncio.to_thread(adapter.asset, safe)
    return Response(
        content=data,
        media_type=mime,
        headers={
            "Cache-Control": IMMUTABLE,
            # 必须是稳定哈希：内置 hash() 按 PYTHONHASHSEED 盐化，进程重启后 ETag 全变，
            # 客户端（OkHttp/Coil）的强缓存会被整体击穿
            "ETag": f'"{book_id}:asset:{stable_hash(safe)}"',
        },
    )


@router.post("/books/open-folder", response_model=OpenFolderResponse, dependencies=[AuthDep])
async def open_folder(req: OpenFolderRequest, ctx=Depends(get_ctx)) -> OpenFolderResponse:
    """文件夹 → 虚拟图集书（设计 7.5）；同一路径重复调用返回同一 book_id。"""
    if not ctx.config.gallery.folder_as_book:
        raise AppError(ErrorCode.UNSUPPORTED_FORMAT, "服务器已关闭“文件夹作为图集”功能")
    info = await asyncio.to_thread(ctx.gallery.register, req.path, req.recursive)
    return OpenFolderResponse(
        book_id=str(info["book_id"]),
        title=str(info["title"]),
        format=str(info.get("format", "gallery")),
        page_count=int(info["page_count"]),
        cover_url=str(info.get("cover_url") or f"/api/v1/books/{info['book_id']}/cover"),
        mode="fixed",
    )
