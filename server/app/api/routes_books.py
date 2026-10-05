"""书籍元数据、封面、书内资源与文件夹图集端点（设计 5.2 #3 / #4 / #10 / #13）。"""

from __future__ import annotations

import asyncio

from fastapi import APIRouter, Depends, Header, Query, Response

from ..models.schemas import (
    BookMetaResponse,
    ChapterInfo,
    ErrorCode,
    OpenFolderRequest,
    OpenFolderResponse,
    RenderSupport,
)
from ..render.raster import text_cover
from ..services.access import AccessScope
from ..util.paths import safe_asset_path
from ..util.textutil import stable_hash
from .deps import ScopeDep, get_ctx
from .errors import AppError
from .routes_read import _etag_matches

router = APIRouter(tags=["books"])

COVER_MAX_AGE = 86400
IMMUTABLE = "public, max-age=31536000, immutable"


@router.get("/books/{book_id}/meta", response_model=BookMetaResponse)
async def book_meta(
    book_id: str, scope: AccessScope = ScopeDep, ctx=Depends(get_ctx)
) -> BookMetaResponse:
    """元数据 + 章节目录 + 封面 URL（EPUB 只做容器级解析，设计 7.3）。"""
    ref = await ctx.require_book_async(book_id, scope)
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
        revision=ref.revision,
        title=meta.title or ref.title_guess,
        author=meta.author,
        format=meta.format or ref.format,
        language=meta.language,
        size=int(meta.size or ref.size or 0),
        mode=meta.mode,  # type: ignore[arg-type]
        page_count=meta.page_count,
        cover_url=f"/api/v1/books/{ref.book_id}/cover?rev={ref.revision}",
        render_support=RenderSupport(png=True, json=bool(meta.supports_json)),
        chapters=chapters,
        source=ref.vpath,
    )


@router.get("/books/{book_id}/cover")
async def book_cover(
    book_id: str,
    w: int = Query(default=240, ge=40, le=1024),
    rev: str = Query(
        default="", description="客户端已知修订号（仅作缓存破坏，服务器不信任）"
    ),
    if_none_match: str | None = Header(default=None, alias="If-None-Match"),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> Response:
    """封面图（JPEG/PNG，强缓存）。无内嵌封面时用 Pillow 生成标题封面。

    ``rev`` 查询参数只用于客户端破坏 URL 缓存：ETag 必须用书籍当前的 revision，
    且**解析书籍之后**才能判 304（书被删除/替换后不能凭旧 ETag 继续命中）。
    """
    # 先解析书籍：书不存在 → 404（不能在未解析的情况下返回 304）
    ref = await ctx.require_book_async(book_id, scope)
    current_rev = ref.revision
    key = ("cover", ref.book_id, current_rev, int(w))
    etag = f'"{ref.book_id}:{current_rev}:w{int(w)}"'
    # 封面列表一次拉几十张：支持条件请求，命中直接 304，
    # 与 get_page 的 304 契约保持一致
    if _etag_matches(if_none_match, etag):
        return Response(
            status_code=304,
            headers={
                "Cache-Control": f"public, max-age={COVER_MAX_AGE}",
                "ETag": etag,
            },
        )
    cached = ctx.cover_cache.get(key)
    headers = {
        "Cache-Control": f"public, max-age={COVER_MAX_AGE}",
        "ETag": etag,
    }
    if cached is not None:
        body, mime = cached
        return Response(content=body, media_type=mime, headers=headers)

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


@router.get("/books/{book_id}/asset")
async def book_asset(
    book_id: str,
    path: str = Query(..., description="容器内资源路径（EPUB 内嵌图片等）"),
    if_none_match: str | None = Header(default=None, alias="If-None-Match"),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> Response:
    """书内资源（只允许取书籍容器内部资源，做规范化校验，设计 9）。"""
    # 先校验书籍可见性：否则其他书库的 book_id 会命中 304 分支泄露资源存在性
    ref = await ctx.require_book_async(book_id, scope)
    safe = safe_asset_path(path)
    # ETag / URL 带上内容修订号：等长就地编辑（book_id 不变、rev 变）后
    # 内嵌图片的强缓存必须失效，否则 JSON 模式会一直显示旧图
    asset_etag = f'"{book_id}:{ref.revision}:asset:{stable_hash(safe)}"'
    if _etag_matches(if_none_match, asset_etag):
        return Response(
            status_code=304,
            headers={"Cache-Control": IMMUTABLE, "ETag": asset_etag},
        )
    async with ctx.open_adapter_async(ref) as adapter:
        data, mime = await asyncio.to_thread(adapter.asset, safe)
    return Response(
        content=data,
        media_type=mime,
        headers={
            "Cache-Control": IMMUTABLE,
            # 必须是稳定哈希：内置 hash() 按 PYTHONHASHSEED 盐化，进程重启后 ETag 全变，
            # 客户端（OkHttp/Coil）的强缓存会被整体击穿
            "ETag": asset_etag,
        },
    )


@router.post("/books/open-folder", response_model=OpenFolderResponse)
async def open_folder(
    req: OpenFolderRequest, scope: AccessScope = ScopeDep, ctx=Depends(get_ctx)
) -> OpenFolderResponse:
    """文件夹 → 虚拟图集书（设计 7.5）；同一路径重复调用返回同一 book_id。"""
    if not ctx.config.gallery.folder_as_book:
        raise AppError(ErrorCode.UNSUPPORTED_FORMAT, "服务器已关闭“文件夹作为图集”功能")
    ctx.library.ensure_path_allowed(req.path, scope)
    info = await asyncio.to_thread(ctx.gallery.register, req.path, req.recursive)
    return OpenFolderResponse(
        book_id=str(info["book_id"]),
        title=str(info["title"]),
        format=str(info.get("format", "gallery")),
        page_count=int(info["page_count"]),
        cover_url=str(info.get("cover_url") or f"/api/v1/books/{info['book_id']}/cover"),
        mode="fixed",
    )
