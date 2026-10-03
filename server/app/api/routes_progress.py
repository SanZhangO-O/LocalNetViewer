"""服务器端进度同步（设计 5.2 #12，P4 可选）。"""

from __future__ import annotations

import asyncio
from typing import Optional

from fastapi import APIRouter, Depends, Query

from ..models.schemas import Anchor, ErrorCode, ProgressPutRequest, ProgressRecord
from .deps import AuthDep, get_ctx
from .errors import AppError

router = APIRouter(tags=["progress"])


@router.get("/progress/{book_id}", response_model=ProgressRecord, dependencies=[AuthDep])
async def get_progress(
    book_id: str,
    device_id: Optional[str] = Query(default=None),
    ctx=Depends(get_ctx),
) -> ProgressRecord:
    """取阅读进度；未指定 device_id 时返回该书最近一次进度。"""
    await ctx.require_book_async(book_id)
    record = ctx.progress.get(book_id, device_id) or (
        None if device_id else ctx.progress.get_any(book_id)
    )
    if record is None:
        # "还没读过"是"未找到"，不是"路径非法"：用 404 NOT_FOUND，
        # 客户端才能与 BAD_PATH（路径打错）区分开。
        raise AppError(ErrorCode.NOT_FOUND, "该书暂无进度记录", {"book_id": book_id})
    return _to_record(record)


@router.put("/progress/{book_id}", response_model=ProgressRecord, dependencies=[AuthDep])
async def put_progress(
    book_id: str,
    body: ProgressPutRequest,
    device_id: Optional[str] = Query(default=None),
    ctx=Depends(get_ctx),
) -> ProgressRecord:
    """写入阅读进度（仅写 key-value，不触碰书籍文件）。"""
    await ctx.require_book_async(book_id)
    # 进度写入落盘是同步 IO：放到线程池，避免阻塞事件循环
    record = await asyncio.to_thread(
        ctx.progress.put,
        book_id,
        device_id,
        body.anchor.model_dump(),
        body.page,
        body.ratio,
    )
    return _to_record(record)


def _to_record(record: dict) -> ProgressRecord:
    anchor = record.get("anchor") or {}
    return ProgressRecord(
        book_id=str(record.get("book_id", "")),
        device_id=record.get("device_id"),
        anchor=Anchor(
            chapter=int(anchor.get("chapter", 0)),
            block=int(anchor.get("block", 0)),
            char=int(anchor.get("char", 0)),
        ),
        page=record.get("page"),
        ratio=record.get("ratio"),
        updated_at=str(record.get("updated_at", "")),
    )
