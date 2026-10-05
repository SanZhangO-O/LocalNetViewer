"""阅读端点：layout / pages / pages meta / locate / search（设计 5.2 #5 / #6 / #7 / #8 / #9）。"""

from __future__ import annotations

import asyncio
from typing import Dict, Optional, Tuple

from fastapi import APIRouter, Depends, Header, Query, Response
from fastapi.responses import JSONResponse

from ..formats.base import BookAdapter, BookRef, ParseTimeoutError
from ..layout import anchors as anchors_mod
from ..models.schemas import (
    Anchor,
    ErrorCode,
    LayoutProfile,
    LayoutResponse,
    LocateResponse,
    PageMetaResponse,
    SearchResponse,
)
from ..render.jsonout import build_page_json
from ..render.raster import fixed_page_bytes, render_flow_page, strip_page_bytes
from ..services.access import AccessScope
from .deps import ScopeDep, get_ctx
from .errors import AppError

router = APIRouter(tags=["read"])

PAGE_CACHE_CONTROL = "private, max-age=31536000, immutable"
VALID_FORMATS = ("png", "json")
VALID_FITS = ("viewport", "original")


# ------------------------------------------------------------------ layout
def _mark_orphan(exc: BaseException, adapter: BookAdapter) -> None:
    """超时工作线程仍在使用适配器时，标记它让 open_adapter_async 跳过 close()。"""
    if isinstance(exc, ParseTimeoutError) and exc.orphaned:
        adapter.mark_orphaned()


@router.post("/books/{book_id}/layout", response_model=LayoutResponse)
async def register_layout(
    book_id: str,
    profile: LayoutProfile,
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> LayoutResponse:
    """注册排版参数 → layout_id、总页数、章节起始页（设计 4.6）。"""
    ref = await ctx.require_book_async(book_id, scope)
    async with ctx.open_adapter_async(ref) as adapter:
        try:
            result = await asyncio.to_thread(ctx.layouts.build, ref, adapter, profile)
        except ParseTimeoutError as exc:
            _mark_orphan(exc, adapter)
            raise
    return LayoutResponse(
        layout_id=result.layout_id,
        revision=ref.revision,
        mode=result.mode,  # type: ignore[arg-type]
        total_pages=result.total_pages,
        chapters=result.chapters,
        cache_hit=result.cache_hit,
        strip=result.strip,
    )


# ------------------------------------------------------------------- pages
def _check_page(entry, page: int) -> None:
    if page < 1 or page > entry.pagemap.total_pages:
        raise AppError(
            ErrorCode.PAGE_OUT_OF_RANGE,
            f"页码越界：{page} / {entry.pagemap.total_pages}",
            {"page": page, "total_pages": entry.pagemap.total_pages},
        )


def _asset_loader(adapter: BookAdapter):
    cache: Dict[str, Optional[bytes]] = {}

    def load(path: str) -> Optional[bytes]:
        if path in cache:
            return cache[path]
        try:
            data, _ = adapter.asset(path)
        except Exception:
            data = None
        cache[path] = data
        return data

    return load


def _render_page(
    ctx,
    ref: BookRef,
    adapter: BookAdapter,
    entry,
    page: int,
    *,
    fmt: str,
    theme: str,
    scale: float,
    fit: str,
    quality: Optional[int],
    pair: bool,
    rtl: bool,
) -> Tuple[bytes, str]:
    """在线程池中执行的渲染入口（CPU 密集，设计 4.9）。"""
    profile = entry.profile
    record = entry.pagemap.record(page)
    if record is None:
        raise AppError(ErrorCode.PAGE_OUT_OF_RANGE, f"页码越界：{page}")
    q = int(quality if quality is not None else ctx.config.comic.jpeg_quality)

    if entry.pagemap.mode == "flow":
        doc = ctx.layouts.document(ref, adapter)
        return render_flow_page(
            doc,
            entry.pagemap,
            page,
            profile,
            ctx.fonts,
            _asset_loader(adapter),
            theme_name=theme,
            scale=scale,
            max_page_pixels=int(ctx.config.render.max_page_pixels),
            max_source_pixels=int(ctx.config.limits.max_source_pixels),
            jpeg_for_photo_pages=bool(ctx.config.render.jpeg_for_photo_pages),
        )
    if entry.pagemap.mode == "strip":
        return strip_page_bytes(
            adapter,
            record,
            profile,
            fit=fit,
            quality=q,
            scale=scale,
            max_page_pixels=int(ctx.config.render.max_page_pixels),
        )
    return fixed_page_bytes(
        adapter,
        record,
        profile,
        fit=fit,
        quality=q,
        scale=scale,
        pair=pair,
        rtl=rtl,
        max_page_pixels=int(ctx.config.render.max_page_pixels),
    )


def _slice_range(body: bytes, range_header: Optional[str]) -> Tuple[str, bytes, str]:
    """解析 Range 头（设计 4.7：fit=original 支持 206 + Content-Length）。

    返回 ``(kind, chunk, content_range)``，kind ∈ ``none`` / ``ok`` / ``unsatisfiable``：
    - 语法无效（RFC 9110 要求**忽略**）→ ``none``，按 200 返回整页；
    - 语法有效但超出范围 → ``unsatisfiable``，按 416 返回。
    """
    if not range_header or not range_header.lower().startswith("bytes="):
        return "none", b"", ""
    spec = range_header.split("=", 1)[1].split(",")[0].strip()
    if "-" not in spec:
        return "none", b"", ""
    start_s, _, end_s = spec.partition("-")
    total = len(body)
    try:
        if start_s == "":
            if end_s == "":
                return "none", b"", ""
            length = int(end_s)
            start = max(0, total - length)
            end = total - 1
        else:
            start = int(start_s)
            end = int(end_s) if end_s else total - 1
    except ValueError:
        return "none", b"", ""
    if start < 0 or end < 0:
        return "none", b"", ""
    if start >= total:
        return "unsatisfiable", b"", f"bytes */{total}"
    end = min(end, total - 1)
    if end < start:
        return "unsatisfiable", b"", f"bytes */{total}"
    return "ok", body[start : end + 1], f"bytes {start}-{end}/{total}"


def _etag_matches(header_value: Optional[str], etag: str) -> bool:
    """按 RFC 9110 比较 If-None-Match：支持 ``*``、``W/`` 前缀与逗号分隔列表。"""
    if not header_value:
        return False
    value = header_value.strip()
    if value == "*":
        return True
    for part in value.split(","):
        candidate = part.strip()
        if candidate.startswith("W/"):
            candidate = candidate[2:].strip()
        if candidate == etag:
            return True
    return False


@router.get("/books/{book_id}/pages/{page}")
async def get_page(
    book_id: str,
    page: int,
    layout_id: str = Query(..., description="POST /layout 返回的 layout_id"),
    format: str = Query(default="png", description="png(默认) | json"),
    theme: str = Query(default="white"),
    scale: float = Query(default=1.0, ge=0.5, le=2.0),
    fit: str = Query(default="viewport", description="viewport | original（固定版式）"),
    quality: Optional[int] = Query(default=None, ge=1, le=100),
    pair: int = Query(default=0, ge=0, le=1),
    rtl: int = Query(default=0, ge=0, le=1),
    if_none_match: Optional[str] = Header(default=None, alias="If-None-Match"),
    range_header: Optional[str] = Header(default=None, alias="Range"),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> Response:
    """页面内容：PNG（默认）或 JSON；固定版式支持 fit / quality / pair。"""
    fmt = (format or "png").lower()
    if fmt not in VALID_FORMATS:
        raise AppError(ErrorCode.BAD_REQUEST, f"format 只支持 {'/'.join(VALID_FORMATS)}", {"format": format})
    fit_mode = (fit or "viewport").lower()
    if fit_mode not in VALID_FITS:
        raise AppError(ErrorCode.BAD_REQUEST, f"fit 只支持 {'/'.join(VALID_FITS)}", {"fit": fit})

    # 先确认书籍存在（且在当前凭据可见书库内），再确认 layout：
    # 删掉的书应报 BOOK_NOT_FOUND 而不是 LAYOUT_NOT_FOUND
    ref = await ctx.require_book_async(book_id, scope)
    entry = await ctx.require_layout_async(book_id, layout_id)
    _check_page(entry, page)
    is_fixed = entry.pagemap.mode in ("fixed", "strip")

    etag = ctx.page_etag(
        layout_id,
        ref.revision,
        page,
        theme=theme,
        scale=scale,
        fit=fit_mode,
        quality=quality,
        pair=bool(pair),
        rtl=bool(rtl),
        fixed=is_fixed,
    )
    headers = {
        "ETag": etag,
        "Cache-Control": PAGE_CACHE_CONTROL,
        "X-Page-Index": str(page),
        "X-Total-Pages": str(entry.pagemap.total_pages),
        "X-Layout-Id": layout_id,
    }
    if _etag_matches(if_none_match, etag):
        return Response(status_code=304, headers=headers)

    cache_key = ctx.page_cache_key(
        book_id,
        layout_id,
        ref.revision,
        page,
        theme=theme,
        scale=scale,
        fit=fit_mode,
        quality=quality,
        pair=bool(pair),
        rtl=bool(rtl),
        fixed=is_fixed,
    )

    if fmt == "json":
        if entry.pagemap.mode != "flow":
            raise AppError(
                ErrorCode.BAD_REQUEST,
                "固定版式（PDF/漫画/图集）不支持 format=json，请使用 format=png",
                {"mode": entry.pagemap.mode},
            )
        # JSON 模式不占用图片缓存（体积小且需带上布局信息）
        async with ctx.open_adapter_async(ref) as adapter:
            try:
                doc = await asyncio.to_thread(ctx.layouts.document, ref, adapter)
                payload = await asyncio.to_thread(
                    build_page_json,
                    doc,
                    entry.pagemap,
                    page,
                    entry.profile,
                    ctx.fonts,
                    ref.book_id,
                )
            except ParseTimeoutError as exc:
                _mark_orphan(exc, adapter)
                raise
        return JSONResponse(content=payload, headers=headers)

    cached = ctx.page_cache.get(cache_key)
    if cached is not None:
        body, mime = cached
    else:
        async with ctx.open_adapter_async(ref) as adapter:
            try:
                body, mime = await asyncio.to_thread(
                    _render_page,
                    ctx,
                    ref,
                    adapter,
                    entry,
                    page,
                    fmt=fmt,
                    theme=theme,
                    scale=scale,
                    fit=fit_mode,
                    quality=quality,
                    pair=bool(pair),
                    rtl=bool(rtl),
                )
            except ParseTimeoutError as exc:
                _mark_orphan(exc, adapter)
                raise
        ctx.page_cache.put(cache_key, (body, mime), len(body))

    if fit_mode == "original" and _is_fixed_mode(entry):
        headers["Accept-Ranges"] = "bytes"
        kind, chunk, content_range = _slice_range(body, range_header)
        if kind == "ok":
            headers["Content-Range"] = content_range
            return Response(content=chunk, status_code=206, media_type=mime, headers=headers)
        if kind == "unsatisfiable":
            return Response(
                status_code=416,
                headers={**headers, "Content-Range": content_range},
            )
        # kind == "none"：语法无效的 Range 按 RFC 忽略，返回整页 200
    return Response(content=body, media_type=mime, headers=headers)


def _is_fixed_mode(entry) -> bool:
    """固定版式（PDF / 漫画 / 图集 / 条漫）：原图模式需要支持 Range 分块加载。"""
    return entry.pagemap.mode in ("fixed", "strip")


@router.get("/books/{book_id}/pages/{page}/meta", response_model=PageMetaResponse)
async def page_meta(
    book_id: str,
    page: int,
    layout_id: str = Query(...),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> PageMetaResponse:
    """页元数据（锚点、章节名、页码）；客户端翻页停稳后请求一次（防抖）。"""
    entry = await ctx.require_layout_async(book_id, layout_id)
    _check_page(entry, page)
    record = entry.pagemap.record(page)
    assert record is not None
    # 先校验书籍可见性再返回首行文本 / 长宽比：layout_id 与书籍无关（几何哈希），
    # 不校验的话其他书库的 book_id 也能读到正文首行
    ref = await ctx.require_book_async(book_id, scope)
    aspect: Optional[float] = None
    if entry.pagemap.mode in ("fixed", "strip"):
        # 条漫页是"切片"，页号 != 源图序号，必须用记录的源图索引取长宽比
        target_page = record.image_index + 1 if record.image_index >= 0 else page
        async with ctx.open_adapter_async(ref) as adapter:
            try:
                aspect = await asyncio.to_thread(adapter.page_aspect, target_page)
            except Exception:
                aspect = None
    return PageMetaResponse(
        page=page,
        total_pages=entry.pagemap.total_pages,
        anchor=Anchor(chapter=record.anchor[0], block=record.anchor[1], char=record.anchor[2]),
        chapter_index=record.chapter_index,
        chapter_title=record.chapter_title,
        first_line=record.first_line,
        mode=entry.pagemap.mode,  # type: ignore[arg-type]
        aspect=aspect,
    )


# ------------------------------------------------------------------ locate
@router.get("/books/{book_id}/locate", response_model=LocateResponse)
async def locate(
    book_id: str,
    layout_id: str = Query(...),
    chapter: int = Query(default=0, ge=0),
    block: int = Query(default=0, ge=0),
    char: int = Query(default=0, ge=0),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> LocateResponse:
    """锚点 → 页码（重新布局 / 恢复进度用，设计 3.3 / 5.3）。"""
    entry = await ctx.require_layout_async(book_id, layout_id)
    await ctx.require_book_async(book_id, scope)
    data = anchors_mod.locate(entry.pagemap, (chapter, block, char))
    return LocateResponse(**data)


# ------------------------------------------------------------------ search
@router.get("/books/{book_id}/search", response_model=SearchResponse)
async def search(
    book_id: str,
    q: str = Query(..., min_length=1),
    layout_id: Optional[str] = Query(default=None),
    limit: int = Query(default=50, ge=1, le=500),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> SearchResponse:
    """全文搜索（TXT/EPUB）；无 layout_id 时 page 为 null（设计 5.3）。"""
    ref = await ctx.require_book_async(book_id, scope)
    pagemap = None
    if layout_id:
        entry = await asyncio.to_thread(ctx.layouts.entry, book_id, layout_id)
        if entry is None:
            raise AppError(
                ErrorCode.LAYOUT_NOT_FOUND,
                "layout_id 不存在或已失效，请重新 POST /layout",
                {"layout_id": layout_id},
            )
        if entry.pagemap.mode != "flow":
            raise AppError(ErrorCode.UNSUPPORTED_FORMAT, "该格式暂不支持全文搜索")
        pagemap = entry.pagemap

    async with ctx.open_adapter_async(ref) as adapter:
        if adapter.mode != "flow":
            raise AppError(
                ErrorCode.UNSUPPORTED_FORMAT,
                f"{adapter.format} 暂不支持全文搜索",
                {"format": adapter.format},
            )
        try:
            doc = await asyncio.to_thread(ctx.layouts.document, ref, adapter)
        except ParseTimeoutError as exc:
            _mark_orphan(exc, adapter)
            raise
    # 全文扫描是 CPU 密集操作（10MB TXT 可能几百毫秒），必须离开事件循环（设计 4.9）
    data = await asyncio.to_thread(ctx.search.search, doc, q, limit=limit, pagemap=pagemap)
    return SearchResponse(**data)
