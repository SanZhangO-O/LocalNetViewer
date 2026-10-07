"""DjVu 适配器（M5 Phase 4）：DjVuLibre 逐页转 JPEG 后按固定版式阅读。

- 页数：``djvused <file> -e n``；
- 每页：``ddjvu -format=jpeg -quality=85 -page=N <file> <out.jpg>``，产物缓存
  在 ``cache/converted/djvu/<book_id>/``；
- 两个工具（ddjvu + djvused，DjVuLibre 同包）任一缺失即置灰。
"""

from __future__ import annotations

import shutil
import subprocess
from functools import lru_cache
from pathlib import Path
from typing import Optional, Tuple

from ..util import imaging
from ..util.paths import is_safe_id, stem_of
from .base import (
    AdapterEnv,
    AdapterMeta,
    BookRef,
    FixedAdapter,
    ParseError,
    UnsupportedFormatError,
)


def djvu_tools() -> Tuple[Optional[str], Optional[str]]:
    """返回 (ddjvu, djvused) 路径；探测结果供 DjvuAdapter 与 registry 共用。"""
    return shutil.which("ddjvu"), shutil.which("djvused")


@lru_cache(maxsize=256)
def _run_page_count(djvused: str, path: str, size: int, mtime_ns: int, timeout: float) -> int:
    """djvused 页数查询（进程级缓存：同一文件每请求不再反复 spawn）。"""
    try:
        proc = subprocess.run(
            [djvused, path, "-e", "n"],
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise ParseError(f"DjVu 页数读取超时（>{timeout:.0f}s）") from exc
    except OSError as exc:
        raise ParseError(f"djvused 无法执行: {exc}") from exc
    if proc.returncode != 0:
        raise ParseError(
            f"djvused 读取页数失败（exit={proc.returncode}），详情见服务器日志"
        )
    try:
        return int((proc.stdout or b"").decode("ascii", errors="ignore").strip() or "0")
    except ValueError as exc:
        raise ParseError("djvused 输出无法解析页数") from exc


class DjvuAdapter(FixedAdapter):
    format = "djvu"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "djvu").lower()
        self._ddjvu, self._djvused = djvu_tools()
        if not self._ddjvu or not self._djvused:
            raise UnsupportedFormatError(
                "DjVu 支持需要 DjVuLibre（ddjvu + djvused），请安装后重试",
                reason="需安装 DjVuLibre",
            )
        if not is_safe_id(ref.book_id):
            raise ParseError(f"非法的 book_id: {ref.book_id}")
        self._cache_dir = env.config.cache_dir / "converted" / "djvu" / ref.book_id
        self._count: Optional[int] = None
        self._size_cache: dict = {}

    def close(self) -> None:
        return None

    # ------------------------------------------------------------ 页数
    def page_count(self) -> int:
        if self._count is None:
            timeout = max(30.0, float(self.env.config.limits.parse_timeout_s) * 2)
            try:
                st = self.ref.path.stat()
                size, mtime_ns = int(st.st_size), int(st.st_mtime_ns)
            except OSError:
                size, mtime_ns = 0, 0
            self._count = _run_page_count(
                self._djvused, str(self.ref.path), size, mtime_ns, timeout
            )
        return self._count

    # ------------------------------------------------------------ 渲染
    def _render(self, page: int) -> Path:
        n = self.page_count()
        if page < 1 or page > n:
            raise UnsupportedFormatError(f"页码越界: {page}")
        target = self._cache_dir / f"{page:05d}.jpg"
        try:
            if target.is_file() and target.stat().st_size > 0:
                return target
        except OSError:
            pass
        try:
            self._cache_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            raise ParseError(f"DjVu 缓存目录无法创建: {exc}") from exc
        timeout = max(60.0, float(self.env.config.limits.parse_timeout_s) * 2)
        tmp = target.with_suffix(f".tmp{page}.jpg")
        try:
            proc = subprocess.run(
                [
                    self._ddjvu,
                    "-format=jpeg",
                    "-quality=85",
                    "-page", str(page),
                    str(self.ref.path),
                    str(tmp),
                ],
                capture_output=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            tmp.unlink(missing_ok=True)
            raise ParseError(f"DjVu 第 {page} 页渲染超时（>{timeout:.0f}s）") from exc
        except OSError as exc:
            tmp.unlink(missing_ok=True)
            raise ParseError(f"ddjvu 无法执行: {exc}") from exc
        if proc.returncode != 0 or not tmp.is_file():
            tmp.unlink(missing_ok=True)
            raise ParseError(f"DjVu 第 {page} 页渲染失败（exit={proc.returncode}）")
        try:
            tmp.replace(target)
        except OSError:
            pass
        return target

    # ------------------------------------------------------------ 取页
    def page_bytes(
        self,
        page: int,
        *,
        fit: str = "viewport",
        quality: int = 85,
        viewport_w: int = 1080,
        render_scale: float = 1.0,
    ) -> Tuple[bytes, str]:
        path = self._render(page)
        data = path.read_bytes()
        target_w = max(1, int(round(viewport_w * max(0.5, min(2.0, render_scale)))))
        try:
            size = self.page_native_size(page)
        except Exception:
            size = (0, 0)
        if fit == "viewport" and size[0] > target_w:
            try:
                img = imaging.decode_for_display(data, target_w, self.env.config.limits.max_source_pixels)
                if img.width > target_w:
                    img = imaging.fit_width(img, target_w)
                return imaging.encode_image(img, prefer_jpeg=True, quality=quality or 85)
            except Exception:
                pass
        return data, "image/jpeg"

    def page_native_size(self, page: int) -> Tuple[int, int]:
        if page in self._size_cache:
            return self._size_cache[page]
        path = self._render(page)
        size = imaging.probe_size(path.read_bytes()) or (0, 0)
        self._size_cache[page] = size
        return size

    def page_aspect(self, page: int) -> float:
        try:
            w, h = self.page_native_size(page)
            return (h / w) if w else 0.0
        except Exception:
            return 0.0

    # ------------------------------------------------------------ meta/封面
    def meta(self) -> AdapterMeta:
        return AdapterMeta(
            title=stem_of(self.ref.name),
            format=self.format,
            mode="fixed",
            author=None,
            language=None,
            size=self.ref.size,
            page_count=self.page_count(),
            chapters=[],
            supports_json=False,
        )

    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        if self.page_count() < 1:
            return None
        try:
            return self.page_bytes(1, fit="viewport", quality=80, viewport_w=max(80, width or 320))
        except Exception:
            return None
