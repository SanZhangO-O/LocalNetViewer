"""FastAPI 装配、路由注册与生命周期（设计 4.3）。

启动顺序：
1. 加载并校验配置 → 创建 :class:`AppContext`（字体、三级缓存、服务）；
2. 读取书库索引（``cache/index.json``）；
3. lifespan 启动后**后台线程**扫描书库（未就绪时 ``/files`` 返回已索引部分）。
"""

from __future__ import annotations

import asyncio
import logging
import os
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import APIRouter, FastAPI, Request

from . import __version__
from .api import (
    routes_books,
    routes_files,
    routes_progress,
    routes_read,
    routes_server,
)
from .api.errors import install_exception_handlers
from .api.deps import AuthDep
from .config import AppConfig, ensure_dirs, load_config
from .context import AppContext

logger = logging.getLogger("localnetviewer")

DESCRIPTION = """LocalNetViewer 局域网电子书阅读服务器。

服务器负责书库管理、格式解析、分页与页面渲染；客户端只按页取图，永不整本下载。
"""


def create_app(
    config: Optional[AppConfig] = None,
    *,
    background_scan: bool = True,
) -> FastAPI:
    """装配应用。

    ``background_scan=False`` 用于 run.py 已经同步预扫描过的场景，避免启动时重复遍历书库。
    """
    cfg = config or load_config()
    ensure_dirs(cfg)
    ctx = AppContext(cfg)
    ctx.library.load_index()
    # 配置了令牌时关闭交互式文档：设计 9 要求"配置 auth_token 后所有请求校验"
    docs_enabled = not cfg.auth_required

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        # 渲染/解析都走 asyncio.to_thread 的默认线程池：显式按 CPU 核数限制
        # （设计 4.9），避免 asyncio 默认的上限在高并发下把重活无限塞进线程池；
        # 但同一线程池也承载阻塞式的适配器打开/关闭（CBR 起 unrar、MOBI 起
        # ebook-convert），必须留出 IO 余量——只按 CPU 核数取 4 时，几个慢打开
        # 就能占满全部 worker，饿死所有人的翻页。下限 8 兼顾两者。
        # 超时执行器（util.concurrency）是独立实现，不受影响。
        asyncio.get_running_loop().set_default_executor(
            ThreadPoolExecutor(max_workers=max(8, os.cpu_count() or 4))
        )
        # lazy 模式（library.scan_mode=lazy，默认）不做任何全量扫描：索引由 /files
        # 按目录增量补齐，因此不启动后台扫描任务。
        effective_scan = background_scan and cfg.library.scan_mode != "lazy"
        scan_task = (
            asyncio.create_task(ctx.library.scan_async(), name="library-scan")
            if effective_scan
            else None
        )
        # 书库目录变化监听（设计 4.8 / 可选依赖 watchfiles）：启动失败只降级记日志。
        # lazy 模式下禁用——watchfiles 的每次变化都会触发一次全量重扫，与会按需
        # 扫描的语义冲突（目录变化由下次列出该目录时自然发现）。
        if cfg.library.scan_mode == "lazy":
            logger.info("书库监控未启用：library.scan_mode=lazy（按需扫描）")
        else:
            ctx.watcher.start()
        logger.info(
            "启动完成：%s（%d 个书库根，索引中 %d 本）",
            cfg.config_path or "(默认配置)",
            len(cfg.library.roots),
            ctx.library.book_count(),
        )
        try:
            yield
        finally:
            # 必须先停监听再取消扫描：监听线程触发的 scan_blocking 会清掉取消
            # 标记，反过来的顺序可能让关停被一次"新启动的"全量扫描拖住
            ctx.watcher.stop()
            ctx.library.cancel_scan()
            if scan_task is not None:
                scan_task.cancel()
                try:
                    await scan_task
                except (asyncio.CancelledError, Exception):  # noqa: BLE001
                    pass
            # 线程无法被强杀，等它协作式退出后再保存索引，避免两个线程写同一个文件
            await asyncio.to_thread(ctx.library.wait_idle, 10.0)
            # index.json 可能几 MB：落盘不能阻塞事件循环
            await asyncio.to_thread(ctx.library.save_index)
            # 进度落盘是合并写入的，关停时必须强制刷一次，否则最后几次翻页会丢
            await asyncio.to_thread(ctx.progress.flush)
            logger.info("服务器已停止")

    app = FastAPI(
        title="LocalNetViewer API",
        version=__version__,
        description=DESCRIPTION,
        lifespan=lifespan,
        docs_url="/docs" if docs_enabled else None,
        redoc_url="/redoc" if docs_enabled else None,
        openapi_url="/openapi.json" if docs_enabled else None,
    )
    app.state.ctx = ctx
    install_exception_handlers(app)

    @app.middleware("http")
    async def _access_log(request: Request, call_next):
        started = time.perf_counter()
        response = await call_next(request)
        elapsed = (time.perf_counter() - started) * 1000.0
        # 只记录路径与耗时，不记录书籍内容（设计 9）
        logger.info(
            "%s %s -> %s %.0fms",
            request.method,
            request.url.path,
            response.status_code,
            elapsed,
        )
        return response

    api = APIRouter(prefix="/api/v1")
    api.include_router(routes_server.router)
    api.include_router(routes_files.router)
    api.include_router(routes_books.router)
    api.include_router(routes_read.router)
    api.include_router(routes_progress.router)
    app.include_router(api)

    # 说明：/ 与 /healthz 只返回服务名/版本/健康状态，不含任何书库数据，但仍按
    # 设计 9 的"配置令牌后所有请求校验"加上 AuthDep；/docs 等在鉴权开启时直接关闭。
    @app.get("/", include_in_schema=False, dependencies=[AuthDep])
    async def root() -> dict:
        return {
            "name": cfg.server.name or "LocalNetViewer",
            "version": __version__,
            "api": "/api/v1",
            "auth_required": cfg.auth_required,
        }

    @app.get("/healthz", include_in_schema=False, dependencies=[AuthDep])
    async def healthz() -> dict:
        return {"ok": True, "scanning": ctx.library.scanning}

    return app
