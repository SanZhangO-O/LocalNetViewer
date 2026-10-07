"""音视频媒体流端点（M5 Phase 5）。

- 仅允许书库根目录内的音频/视频文件（访问范围校验与 /files 一致）；
- 支持 HTTP Range（206 / 416），播放器可拖进度；整段不落内存，按块流式输出；
- ETag + 条件请求支持 304（播放器重复探测时省流量）。
"""

from __future__ import annotations

import asyncio
import mimetypes
from typing import Iterator

from fastapi import APIRouter, Depends, Header, Query, Response
from fastapi.responses import StreamingResponse

from ..formats import registry
from ..models.schemas import ErrorCode
from ..services.access import AccessScope
from ..util import imaging
from ..util.httprange import parse_range_header
from ..util.paths import ext_of
from ..util.textutil import stable_hash
from .deps import ScopeDep, get_ctx
from .errors import AppError
from .routes_read import _etag_matches

router = APIRouter(tags=["media"])

CHUNK_BYTES = 256 * 1024


def _iter_file(path, start: int, length: int) -> Iterator[bytes]:
    with open(path, "rb") as fh:
        fh.seek(start)
        remaining = int(length)
        while remaining > 0:
            chunk = fh.read(min(CHUNK_BYTES, remaining))
            if not chunk:
                break
            remaining -= len(chunk)
            yield chunk


@router.get("/media")
async def media_stream(
    path: str = Query(..., description="书库内虚拟路径"),
    if_none_match: str | None = Header(default=None, alias="If-None-Match"),
    range_header: str | None = Header(default=None, alias="Range"),
    scope: AccessScope = ScopeDep,
    ctx=Depends(get_ctx),
) -> Response:
    """音频/视频流（Range 支持）。"""
    ctx.library.ensure_path_allowed(path, scope)
    real = await asyncio.to_thread(ctx.library.resolve_real, path)
    if real is None or not real.is_file():
        raise AppError(ErrorCode.NOT_FOUND, "媒体文件不存在", {"path": path})
    fmt = registry.format_of(real.name)
    spec = registry.spec_of(fmt or "")
    if spec is None or spec.kind not in ("audio", "video"):
        raise AppError(
            ErrorCode.UNSUPPORTED_FORMAT,
            "不是可流式播放的媒体文件",
            {"format": fmt or ""},
        )
    try:
        st = real.stat()
    except OSError:
        raise AppError(ErrorCode.NOT_FOUND, "媒体文件不存在", {"path": path})
    size = int(st.st_size)
    mtime = float(st.st_mtime)
    ext = ext_of(real.name)
    mime = imaging.mime_for_ext(ext)
    if mime == "application/octet-stream":
        mime = mimetypes.guess_type(real.name)[0] or mime
    etag = f'"{stable_hash(f"{path}:{size}:{int(mtime)}")}"'
    headers = {
        "ETag": etag,
        "Cache-Control": "private, max-age=3600",
        "Accept-Ranges": "bytes",
    }
    if _etag_matches(if_none_match, etag):
        return Response(status_code=304, headers=headers)

    if size <= 0:
        # 空文件：按 200 返回空体（Range 语义对 0 长度没有可满足区间）
        return StreamingResponse(
            _iter_file(real, 0, 0),
            media_type=mime,
            headers={**headers, "Content-Length": "0"},
        )

    kind, start, end = parse_range_header(range_header, size)
    if kind == "unsatisfiable":
        return Response(
            status_code=416,
            headers={**headers, "Content-Range": f"bytes */{size}"},
        )
    if kind == "ok":
        length = end - start + 1
        return StreamingResponse(
            _iter_file(real, start, length),
            status_code=206,
            media_type=mime,
            headers={
                **headers,
                "Content-Range": f"bytes {start}-{end}/{size}",
                "Content-Length": str(length),
            },
        )
    return StreamingResponse(
        _iter_file(real, 0, size),
        media_type=mime,
        headers={**headers, "Content-Length": str(size)},
    )
