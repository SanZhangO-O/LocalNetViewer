"""缓存失效（设计 10.3：缓存失效）。

核心回归：``book_id = sha1(大小 + 文件头 1MB)``。对 1MB 以上的文件在 1MB **之后**
做等长编辑（TXT 勘误的常见形态），指纹算出来和旧的一模一样 —— 如果不额外检测
size/mtime 变化并清空派生缓存，服务器会永远返回旧分页与旧页面图。
"""

from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from app.layout.engine import PageMap, PageRecord
from app.models.schemas import LayoutProfile, Margin, FontSpec, Viewport
from app.services.library import compute_book_id
from tests.conftest import vpath_of

HEAD = 1 << 20  # 1MB，与 library.HEAD_BYTES 一致


def _write_big_txt(path: Path, marker: bytes = b"B") -> None:
    """写一个 >1MB 的 TXT：正文 + 1MB 填充 + 尾部标记。"""
    body = ("第一章 开端\n" + "这是正文段落，用来产生可读的章节结构。\n" * 40).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body + b"A" * HEAD + marker * 4096)


def _edit_beyond_head(path: Path, marker: bytes = b"C") -> None:
    """在 1MB 之后做**等长**编辑（文件总大小不变）。"""
    with open(path, "r+b") as fh:
        fh.seek(HEAD + 1024)
        fh.write(marker * 1024)
        fh.flush()
        os.fsync(fh.fileno())


def test_edit_beyond_head_keeps_book_id(tmp_books: Path):
    """先证明"这个洞是真的"：1MB 之后的等长编辑不改变 book_id。"""
    path = tmp_books / "tale.txt"
    _write_big_txt(path)
    before = compute_book_id(path, path.stat().st_size)

    time.sleep(0.02)
    _edit_beyond_head(path)
    after = compute_book_id(path, path.stat().st_size)

    assert path.stat().st_size > HEAD
    assert before == after, "测试前提失效：指纹居然能区分这次编辑"


def _populate_caches(ctx, book_id: str, layout_id: str) -> None:
    """把三级缓存都填上这个 book_id 的痕迹。"""
    from app.cache.store import LayoutEntry

    pagemap = PageMap(
        book_id=book_id,
        layout_id=layout_id,
        mode="flow",
        total_pages=1,
        pages=[PageRecord(page=1, anchor=(0, 0, 0))],
        end_anchor=(0, 1, 0),
    )
    profile = LayoutProfile(
        viewport=Viewport(w=100, h=100),
        margin=Margin(),
        font=FontSpec(),
    )
    ctx.layout_store.save(LayoutEntry(book_id, layout_id, profile, pagemap))

    from app.formats.base import Chapter, DocumentModel

    model = DocumentModel(book_id=book_id, format="txt")
    model.chapters.append(Chapter(index=0, title="t", blocks=[]))
    ctx.parsed.save_disk(model)
    ctx.parsed.memory.put(book_id, model)

    ctx.page_cache.put(
        (book_id, layout_id, "rev-test", 1, "white", 1.0), (b"page", "image/png"), 4
    )
    ctx.cover_cache.put(("cover", book_id, "rev-test", 240), (b"cover", "image/jpeg"), 5)


def _assert_caches_cleared(ctx, book_id: str, layout_id: str) -> None:
    assert ctx.layout_store.get(book_id, layout_id) is None, "布局缓存未失效"
    assert ctx.parsed.load_disk(book_id) is None, "解析缓存未失效"
    assert ctx.parsed.get_memory(book_id) is None, "解析内存缓存未失效"
    assert (
        ctx.page_cache.get((book_id, layout_id, "rev-test", 1, "white", 1.0)) is None
    ), "页面缓存未失效"
    assert ctx.cover_cache.get(("cover", book_id, "rev-test", 240)) is None, "封面缓存未失效"


def test_same_size_edit_invalidates_all_derived_caches(context, tmp_books: Path):
    """1MB 之后的等长编辑必须清空布局 / 解析 / 页面 / 封面缓存。"""
    path = tmp_books / "tale.txt"
    _write_big_txt(path)
    context.library.scan_blocking()

    ref = context.library.book_by_vpath(vpath_of("tale.txt"))
    assert ref is not None
    layout_id = "Lcafebabe0000"
    _populate_caches(context, ref.book_id, layout_id)

    time.sleep(0.02)
    _edit_beyond_head(path)

    # 触发内容变化检测
    ref2 = context.require_book(ref.book_id)
    assert ref2.book_id == ref.book_id
    _assert_caches_cleared(context, ref.book_id, layout_id)


def test_content_change_does_not_fire_without_change(context, tmp_books: Path):
    """没变化时不能误清缓存（否则每次请求都重新解析，性能崩掉）。"""
    path = tmp_books / "stable.txt"
    _write_big_txt(path)
    context.library.scan_blocking()
    ref = context.library.book_by_vpath(vpath_of("stable.txt"))
    assert ref is not None
    layout_id = "Ldeadbeef0000"
    _populate_caches(context, ref.book_id, layout_id)

    for _ in range(3):
        context.require_book(ref.book_id)

    assert context.layout_store.get(ref.book_id, layout_id) is not None
    assert context.parsed.load_disk(ref.book_id) is not None


def test_head_change_yields_book_not_found(context, tmp_books: Path):
    """文件头变了 → 指纹变了 → 旧 book_id 应当 404，客户端重新扫描拿新 id。"""
    from app.api.errors import AppError

    path = tmp_books / "swap.txt"
    _write_big_txt(path)
    context.library.scan_blocking()
    ref = context.library.book_by_vpath(vpath_of("swap.txt"))
    assert ref is not None

    time.sleep(0.02)
    path.write_bytes(b"COMPLETELY DIFFERENT CONTENT\n" * 100)

    with pytest.raises(AppError) as excinfo:
        context.require_book(ref.book_id)
    assert excinfo.value.code == "BOOK_NOT_FOUND"


def test_invalidation_listener_is_wired(context):
    """AppContext 必须把"内容变化"接到缓存失效上（不能是死代码）。"""
    seen = []
    context.library.add_content_listener(seen.append)
    context.invalidate_book_caches("nonexistentbook")
    # 直接调用也要能工作且不抛异常
    context.library._notify_content_changed("book-x")
    assert seen == ["book-x"]


def test_end_to_end_layout_cache_hit_flips_after_edit(config, tmp_books: Path):
    """HTTP 层验证：编辑后 POST /layout 必须重建（cache_hit 从 True 变回 False）。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    path = tmp_books / "tale.txt"
    _write_big_txt(path)

    app = create_app(config, background_scan=False)
    with TestClient(app) as client:
        ctx = app.state.ctx
        ctx.library.scan_blocking()
        ref = ctx.library.book_by_vpath(vpath_of("tale.txt"))
        assert ref is not None
        book_id = ref.book_id

        profile = {
            "viewport": {"w": 800, "h": 1200},
            "margin": {"left": 20, "right": 20, "top": 20, "bottom": 60},
            "font": {"family": "auto", "size_px": 40, "line_height": 1.6},
            "paragraph": {"spacing_px": 10, "first_line_indent_chars": 2},
            "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
            "chapter_new_page": True,
            "mode": "flow",
        }
        first = client.post(f"/api/v1/books/{book_id}/layout", json=profile)
        assert first.status_code == 200, first.text
        assert first.json()["cache_hit"] is False
        layout_id = first.json()["layout_id"]

        again = client.post(f"/api/v1/books/{book_id}/layout", json=profile)
        assert again.json()["cache_hit"] is True, "同一 layout 第二次应当命中缓存"

        page_before = client.get(
            f"/api/v1/books/{book_id}/pages/1", params={"layout_id": layout_id}
        )
        assert page_before.status_code == 200

        time.sleep(0.02)
        _edit_beyond_head(path)

        # 任意一次 require_book 都会触发检测；这里直接走 /pages
        refreshed = client.post(f"/api/v1/books/{book_id}/layout", json=profile)
        assert refreshed.status_code == 200, refreshed.text
        assert refreshed.json()["cache_hit"] is False, (
            "内容变化后仍报缓存命中 —— 失效没有接线"
        )


def test_book_by_id_falls_back_to_moved_vpath(tmp_books: Path, config):
    """文件移动后旧索引残留同指纹条目：id→旧路径失效时兜底到新路径。

    现场（LESSONS.md 2026-10-05）：文件从 /lib/old/book.txt 移到 /lib/sub/book.txt，
    lazy 模式不重扫，index.json 同时残留两条同 book_id 条目；load_index 的
    id→vpath 同指纹后写者覆盖可能让失效旧路径胜出 → 列表可见、打开永远 404。
    """
    from app.context import AppContext

    (tmp_books / "sub").mkdir(parents=True, exist_ok=True)
    (tmp_books / "sub" / "book.txt").write_text(
        "第一章 移动\n" + "正文内容。" * 100, encoding="utf-8"
    )

    ctx = AppContext(config)
    ctx.library.scan_blocking()
    lib = ctx.library
    live_vpath = vpath_of("sub/book.txt")
    entry = lib._books.get(live_vpath)
    assert entry and entry.get("book_id"), "测试前提失效：书未被扫描进索引"
    bid = str(entry["book_id"])

    # 注入"移动后残留"的索引状态：旧路径同指纹条目 + id 指向旧路径
    dead_vpath = vpath_of("old/book.txt")
    lib._books[dead_vpath] = dict(entry)
    lib._by_id[bid] = dead_vpath

    ref = lib.book_by_id(bid)
    assert ref is not None, "同指纹兜底失效：移动过的书打不开了"
    assert ref.vpath == live_vpath
    # 命中兜底后把 id 晋升回活跃路径
    assert lib._by_id[bid] == live_vpath
