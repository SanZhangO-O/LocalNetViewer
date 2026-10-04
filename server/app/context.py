"""应用上下文：依赖容器（设计 4.3 的装配层）。

把配置、字体、三级缓存、服务与适配器工厂串起来，避免各层互相 import 造成环。
"""

from __future__ import annotations

import asyncio
import logging
import time
from contextlib import asynccontextmanager
from typing import Any, AsyncIterator, Dict, Optional

from .cache.lru import PageLRU
from .cache.parsed import ParsedCache
from .cache.store import LayoutStore
from .config import AppConfig, ensure_dirs
from .formats import registry
from .formats.base import AdapterEnv, BookAdapter, BookRef
from .formats.mobi import sweep_converted_cache
from .layout.fonts import FontManager
from .models.schemas import ErrorCode
from .services.gallery import GalleryService
from .services.layouts import LayoutService
from .services.library import LibraryService
from .services.progress import ProgressStore
from .services.search import SearchService
from .util import imaging
from .api.errors import AppError

logger = logging.getLogger("localnetviewer")


class AppContext:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        ensure_dirs(config)
        imaging.set_pixel_limit(config.limits.max_source_pixels)

        self.fonts = FontManager(config)
        self.parsed = ParsedCache(config)
        self.layout_store = LayoutStore(config)
        self.page_cache = PageLRU(
            max_bytes=config.cache.page_lru_bytes, max_pages=config.cache.page_lru_pages
        )
        self.cover_cache = PageLRU(max_bytes=64 * 1024 * 1024, max_pages=256)

        self.library = LibraryService(config)
        self.gallery = GalleryService(config, self.library)
        self.library.gallery = self.gallery
        self.layouts = LayoutService(config, self.layout_store, self.parsed, self.fonts)
        self.search = SearchService()
        self.progress = ProgressStore(config)

        self.env = AdapterEnv(config=config, parsed_cache=self.parsed, fonts=self.fonts)
        self.started_at = time.time()

        # MOBI/AZW3 的转换缓存（cache/converted）不属于三级缓存，启动时做一次
        # 按 mtime 的 LRU 配额清理（设计：转换产物要有上限）
        sweep_converted_cache(config.cache_dir / "converted")

        # 书籍内容变化 → 丢弃全部由旧内容派生的缓存（设计 4.6）。
        # 少了这一步，"同尺寸就地编辑"会让服务器永远返回旧分页与旧页面图。
        self.library.add_content_listener(self.invalidate_book_caches)

    # ------------------------------------------------------------ 失效
    def invalidate_book_caches(self, book_id: str) -> None:
        """清空某本书的三级缓存派生数据（布局 / 解析 / 页面图 / 封面）。"""
        removed = 0
        removed += self.layout_store.invalidate_book(book_id)
        self.parsed.invalidate(book_id)
        removed += self.page_cache.invalidate(lambda key: bool(key) and key[0] == book_id)
        # 封面缓存键是 ("cover", book_id, rev, width)
        self.cover_cache.invalidate(
            lambda key: len(key) > 1 and key[0] == "cover" and key[1] == book_id
        )
        # MOBI/AZW3 的转换缓存也必须作废：否则内容换掉后仍解析旧 EPUB。
        # 产物按 revision 命名（{book_id}.{rev}.epub），新 rev 天然指向新文件；
        # 这里再清掉该书全部旧 rev 产物与 .tmp（含在途转换先写的临时文件），
        # 避免竞态窗口里旧产物被再次采用或堆积。
        converted_dir = self.config.cache_dir / "converted"
        try:
            stale = list(converted_dir.glob(f"{book_id}.*.epub*"))
            stale.append(converted_dir / f"{book_id}.epub")
            for path in stale:
                try:
                    path.unlink(missing_ok=True)
                except OSError:
                    pass
        except OSError:
            pass
        if removed:
            logger.info("书籍内容已变化，清理 %d 项缓存：%s", removed, book_id)

    # ------------------------------------------------------------ 适配器
    def create_adapter(self, ref: BookRef) -> BookAdapter:
        return registry.create_adapter(
            self.env, ref, recursive=bool(ref.extra.get("recursive", False))
        )

    @asynccontextmanager
    async def open_adapter_async(self, ref: BookRef) -> AsyncIterator[BookAdapter]:
        """异步版：适配器构造/关闭都可能做重 IO，必须离开事件循环（设计 4.9）。

        构造一个适配器会做的事：CBR 列目录（unrar 子进程）、PDF 打开
        （``PdfDocument``）、MOBI 首次打开（``ebook-convert`` 子进程，最长
        ``parse_timeout_s × 4`` 秒）、ZIP/EPUB 扫描条目表。任何一项直接跑在
        事件循环上，都会把**所有**并发请求（包括别人的翻页）一起冻住。
        """
        adapter = await asyncio.to_thread(self.create_adapter, ref)
        try:
            yield adapter
        finally:
            await asyncio.to_thread(_quiet_close, adapter)

    # ------------------------------------------------------------ 书籍
    def require_book(self, book_id: str) -> BookRef:
        ref = self.library.book_by_id(book_id)
        if ref is None:
            raise AppError(
                ErrorCode.BOOK_NOT_FOUND, "书不存在或已被移除", {"book_id": book_id}
            )
        return ref

    async def require_book_async(self, book_id: str) -> BookRef:
        """异步版：``book_by_id`` 命中"文件被就地替换"分支时会重算 book_id（读 1MB 文件头）。"""
        return await asyncio.to_thread(self.require_book, book_id)

    def require_layout(self, book_id: str, layout_id: str):
        entry = self.layouts.entry(book_id, layout_id)
        if entry is None:
            raise AppError(
                ErrorCode.LAYOUT_NOT_FOUND,
                "layout_id 不存在或已失效，请重新 POST /layout",
                {"book_id": book_id, "layout_id": layout_id},
            )
        return entry

    async def require_layout_async(self, book_id: str, layout_id: str):
        """异步版：缓存未命中时 ``store.get`` 会做 gzip + JSON 磁盘读取。"""
        return await asyncio.to_thread(self.require_layout, book_id, layout_id)

    # ------------------------------------------------------------ 缓存键
    @staticmethod
    def page_cache_key(
        book_id: str,
        layout_id: str,
        rev: str,
        page: int,
        *,
        theme: str = "white",
        scale: float = 1.0,
        fit: str = "viewport",
        quality: Optional[int] = None,
        pair: bool = False,
        rtl: bool = False,
        fixed: bool = False,
    ) -> tuple:
        key = [
            book_id,
            layout_id,
            str(rev or ""),
            int(page),
            theme if not fixed else "",
            round(float(scale), 2),
        ]
        if fixed:
            key.extend([fit, int(quality or 0), 1 if pair else 0, 1 if rtl else 0])
        return tuple(key)

    @staticmethod
    def page_etag(
        layout_id: str,
        rev: str,
        page: int,
        *,
        theme: str = "white",
        scale: float = 1.0,
        fit: str = "viewport",
        quality: Optional[int] = None,
        pair: bool = False,
        rtl: bool = False,
        fixed: bool = False,
    ) -> str:
        # scale 与页面缓存键保持一致的精度：常见值输出 1 位（如 "1.0"），
        # 非整十分位才用 2 位，避免 ETag 与缓存键在小数位上分歧导致错误 304。
        scale_tag = _scale_tag(scale)
        parts = [
            layout_id,
            str(rev or ""),
            theme if not fixed else "-",
            scale_tag,
            str(int(page)),
        ]
        if fixed:
            parts.extend(
                [
                    fit,
                    str(int(quality or 0)),
                    "pair" if pair else "-",
                    "rtl" if rtl else "-",
                ]
            )
        return '"' + ":".join(parts) + '"'

    # ------------------------------------------------------------ 统计
    def stats(self) -> Dict[str, Any]:
        return {
            "uptime_s": round(time.time() - self.started_at, 1),
            "fonts": {
                "count": len(self.fonts.list_faces()),
                "default": self.fonts.default_id,
                "missing": self.fonts.missing_font,
            },
            "page_cache": self.page_cache.stats(),
            "cover_cache": self.cover_cache.stats(),
            "parsed_cache": self.parsed.disk_stats(),
            "layout_cache": self.layout_store.stats(),
            "library": self.library.index_stats(),
            "gallery_books": self.gallery.registry_size(),
            "progress_records": self.progress.count(),
        }

    def startup_report(self) -> str:
        cfg = self.config
        lines = [f"LocalNetViewer Server v{_version()}"]
        if cfg.library.roots:
            for root in cfg.library.roots:
                info = next(
                    (i for i in self.library.libraries_info() if i["name"] == root.name), None
                )
                count = info["book_count"] if info else 0
                state = "扫描中" if self.library.scanning else "扫描完成"
                lines.append(f"书库: {root.name} ({root.path}, {count} 本)  [{state}]")
        else:
            lines.append("书库: 未配置（请编辑 config.yaml 的 library.roots）")
        lines.append(f"字体: {self.fonts.default_id}（{len(self.fonts.list_faces())} 个可用）")
        if self.fonts.missing_font:
            lines.append("警告: 未找到任何字体文件，中文将显示为方框（请放入 assets/fonts）")
        elif not self.fonts.has_cjk_font():
            lines.append("警告: 默认字体可能缺少中文字形，建议在 assets/fonts 放入 Noto Sans SC")
        lines.append(f"令牌: {'已启用' if cfg.auth_required else '未启用'}")
        for warning in self.library.warnings:
            lines.append(f"警告: {warning}")
        backends = registry.backend_status()
        unavailable = [
            f"{name}({data['hint']})" for name, data in backends.items() if not data["available"]
        ]
        if unavailable:
            lines.append("可选后端缺失: " + ", ".join(unavailable))
        return "\n".join(lines)


def _quiet_close(adapter: BookAdapter) -> None:
    """关闭适配器；关闭失败不能影响响应（缓存文件已落盘，句柄由 GC 兜底）。

    ``orphaned``（解析/分页超时、工作线程仍在读适配器）时必须跳过 close()：
    提前关闭会让工作线程读到坏数据；等工作线程释放引用后由 __del__ 兜底关闭。
    """
    if getattr(adapter, "orphaned", False):
        return
    try:
        adapter.close()
        # 正常关闭成功：GC 兜底（__del__）不再重复关闭，保证恰好关闭一次
        adapter.mark_closed()
    except Exception:
        pass


def _version() -> str:
    from . import __version__

    return __version__


def _scale_tag(scale: float) -> str:
    value = round(float(scale or 1.0), 2)
    if abs(value - round(value, 1)) < 1e-9:
        return f"{value:.1f}"
    return f"{value:.2f}"
