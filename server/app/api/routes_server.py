"""服务器信息与字体端点（设计 5.2 #1 / #11 / #14）。"""

from __future__ import annotations

import asyncio
import time

from fastapi import APIRouter, Depends
from fastapi.responses import FileResponse

from ..models.schemas import (
    ErrorCode,
    FeaturesInfo,
    FontInfo,
    FontsResponse,
    LibraryInfo,
    ServerInfoResponse,
)
from .deps import AuthDep, get_ctx
from .errors import AppError

router = APIRouter(tags=["server"])


def _version() -> str:
    from .. import __version__

    return __version__


@router.get("/server/info", response_model=ServerInfoResponse)
async def server_info(ctx=Depends(get_ctx)) -> ServerInfoResponse:
    """服务信息（不需要令牌：客户端要先知道是否需要令牌）。

    这个端点**不鉴权**，因此不能带真实文件系统路径（设计 9 / paths.py 的
    "真实路径永不返回给客户端"）：只给出书库名与藏书数。带路径的完整列表在
    需要令牌的 ``/files`` 里。
    """
    libs = [
        LibraryInfo(name=item["name"], book_count=item["book_count"], path=None)
        for item in ctx.library.libraries_info()
    ]
    return ServerInfoResponse(
        name=ctx.config.server.name or "LocalNetViewer",
        version=_version(),
        api_version="v1",
        auth_required=ctx.config.auth_required,
        libraries=libs,
        features=FeaturesInfo(
            png=True,
            json=True,
            search=True,
            gallery=bool(ctx.config.gallery.folder_as_book),
            strip=True,
            progress_sync=True,
            strip_min_aspect=float(ctx.config.comic.strip_min_aspect),
        ),
        server_time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    )


@router.get("/render/fonts", response_model=FontsResponse, dependencies=[AuthDep])
async def list_fonts(ctx=Depends(get_ctx)) -> FontsResponse:
    """可用字体列表（P2 阅读设置页用）。"""
    infos = [
        FontInfo(
            id=str(item["id"]),
            name=str(item["name"]),
            file_name=str(item["file_name"]),
            default=bool(item["default"]),
            builtin=bool(item["builtin"]),
        )
        for item in ctx.fonts.list_infos()
    ]
    return FontsResponse(default=ctx.fonts.default_id, fonts=infos)


@router.get("/render/fonts/{font_id}/file", dependencies=[AuthDep])
async def font_file(font_id: str, ctx=Depends(get_ctx)):
    """字体文件下载（P4：JSON 模式客户端对齐排版用）。"""
    face = await asyncio.to_thread(ctx.fonts.get, font_id)
    if face is None:
        raise AppError(ErrorCode.ASSET_NOT_FOUND, "字体不存在", {"font_id": font_id})
    return FileResponse(
        path=str(face.path),
        media_type="font/otf" if face.path.suffix.lower() in (".otf", ".otc") else "font/ttf",
        filename=face.path.name,
        headers={"Cache-Control": "public, max-age=31536000, immutable"},
    )
