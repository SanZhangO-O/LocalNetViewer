"""pytest 共享夹具（设计 10.3）。

所有夹具都在 ``tmp_path`` 里造一个自包含环境（书库目录 + 缓存目录 + 字体目录），
不依赖仓库里的 ``config.yaml``，也不联网。
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path
from typing import Callable, Dict, List, Optional

import pytest
from PIL import Image

from app.config import (
    AppConfig,
    ArchiveCfg,
    CacheCfg,
    ComicCfg,
    GalleryCfg,
    LibraryCfg,
    LibraryRootCfg,
    LimitsCfg,
    RenderCfg,
    ServerCfg,
)
from app.formats.base import AdapterEnv, Block, Chapter, DocumentModel
from app.layout.fonts import FontManager
from app.models.schemas import FontSpec, LayoutProfile, Margin, Viewport

BOOKS_DIR_NAME = "books"
ROOT_NAME = "lib"


# --------------------------------------------------------------------- 配置
@pytest.fixture
def tmp_books(tmp_path: Path) -> Path:
    books = tmp_path / BOOKS_DIR_NAME
    books.mkdir(parents=True, exist_ok=True)
    return books


@pytest.fixture
def make_config(tmp_path: Path, tmp_books: Path) -> Callable[..., AppConfig]:
    def _make(**overrides) -> AppConfig:
        cfg = AppConfig(
            server=ServerCfg(name="TestServer", auth_token=overrides.pop("auth_token", "")),
            library=LibraryCfg(
                roots=[LibraryRootCfg(name=ROOT_NAME, path=str(tmp_books))],
                **overrides.pop("library", {}),
            ),
            gallery=GalleryCfg(**overrides.pop("gallery", {})),
            comic=ComicCfg(**overrides.pop("comic", {})),
            archive=ArchiveCfg(**overrides.pop("archive", {})),
            render=RenderCfg(fonts_dir=str(tmp_path / "fonts"), **overrides.pop("render", {})),
            cache=CacheCfg(dir=str(tmp_path / "cache"), **overrides.pop("cache", {})),
            limits=LimitsCfg(**overrides.pop("limits", {})),
        )
        cfg.base_dir = tmp_path
        return cfg

    return _make


@pytest.fixture
def config(make_config: Callable[..., AppConfig]) -> AppConfig:
    return make_config()


# --------------------------------------------------------------------- 字体
_font_manager_cache: Dict[str, FontManager] = {}


@pytest.fixture
def fonts(config: AppConfig) -> FontManager:
    """FontManager 会扫描系统字体目录，比较慢 —— 按配置目录缓存复用。"""
    key = str(config.fonts_dir)
    manager = _font_manager_cache.get(key)
    if manager is None:
        manager = FontManager(config)
        _font_manager_cache[key] = manager
    return manager


# --------------------------------------------------------------------- 适配器环境
@pytest.fixture
def env(config: AppConfig, fonts: FontManager) -> AdapterEnv:
    return AdapterEnv(config=config, parsed_cache=None, fonts=fonts)


# --------------------------------------------------------------------- 排版参数
def make_profile(
    *,
    w: int = 1080,
    h: int = 1920,
    size: int = 52,
    line_height: float = 1.6,
    mode: str = "flow",
    chapter_new_page: bool = True,
    margin: Optional[Margin] = None,
) -> LayoutProfile:
    return LayoutProfile(
        viewport=Viewport(w=w, h=h),
        margin=margin or Margin(left=40, right=40, top=30, bottom=140),
        font=FontSpec(family="auto", size_px=size, line_height=line_height),
        mode=mode,  # type: ignore[arg-type]
        chapter_new_page=chapter_new_page,
    )


@pytest.fixture
def profile() -> LayoutProfile:
    return make_profile()


# --------------------------------------------------------------------- 文档模型
def make_doc(
    *,
    book_id: str = "book000000000001",
    chapters: int = 3,
    blocks_per_chapter: int = 40,
    chars_per_block: int = 220,
    title_prefix: str = "第",
) -> DocumentModel:
    """造一个"中文网文"形态的文档：章节 + 若干段中文正文。"""
    model = DocumentModel(book_id=book_id, format="txt", meta={"title": "测试书"})
    for ci in range(chapters):
        blocks: List[Block] = [Block.heading(f"{title_prefix}{ci + 1}章 测试标题", level=1)]
        for bi in range(blocks_per_chapter):
            body = f"这是第{ci + 1}章第{bi + 1}段。" + ("测试正文内容用于折行。" * (chars_per_block // 12))
            blocks.append(Block.paragraph(body))
        model.chapters.append(
            Chapter(index=ci, title=f"{title_prefix}{ci + 1}章 测试标题", blocks=blocks)
        )
    return model


@pytest.fixture
def doc() -> DocumentModel:
    return make_doc()


# --------------------------------------------------------------------- 图片/容器
def png_bytes(width: int, height: int, color: tuple = (200, 30, 30)) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (max(1, width), max(1, height)), color).save(buf, format="PNG")
    return buf.getvalue()


def jpeg_bytes(width: int, height: int, color: tuple = (30, 60, 200)) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (max(1, width), max(1, height)), color).save(buf, format="JPEG", quality=80)
    return buf.getvalue()


def write_cbz(path: Path, entries: Dict[str, bytes]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in entries.items():
            zf.writestr(name, data)
    return path


@pytest.fixture
def cbz_factory() -> Callable[..., Path]:
    def _make(path: Path, count: int = 3, width: int = 800, height: int = 1200) -> Path:
        entries = {
            f"page_{i + 1:03d}.png": png_bytes(width, height) for i in range(count)
        }
        return write_cbz(path, entries)

    return _make


# --------------------------------------------------------------------- 书籍文件
@pytest.fixture
def make_book(tmp_books: Path) -> Callable[..., Path]:
    def _make(name: str, content: bytes) -> Path:
        path = tmp_books / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    return _make


def vpath_of(name: str) -> str:
    return f"/{ROOT_NAME}/{name}"


# --------------------------------------------------------------------- 服务
@pytest.fixture
def context(config: AppConfig):
    """已扫描书库、可脱离 HTTP 使用的 AppContext。"""
    from app.context import AppContext

    ctx = AppContext(config)
    ctx.library.scan_blocking()
    return ctx


@pytest.fixture
def client(config: AppConfig):
    """FastAPI TestClient（关闭后台扫描，由测试自己控制）。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    app = create_app(config, background_scan=False)
    with TestClient(app) as c:
        yield c
