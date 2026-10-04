"""书库目录变化监听（app/services/watcher.py，设计 4.8）。

监听依赖可选包 watchfiles（pyproject ``watch`` extra）：降级路径用 monkeypatch
覆盖；真实监听用例在缺 watchfiles 的环境自动 skip。防抖窗口用 0.2s、节流间隔用
0.4s 缩短等待；生产默认节流 10s。
"""

from __future__ import annotations

import time
from typing import Callable, List

import pytest

import app.services.watcher as watcher_mod
from app.config import LibraryRootCfg
from app.services.library import LibraryService, compute_book_id
from app.services.watcher import LibraryWatcher

# 与 conftest 的 ROOT_NAME（"lib"）一致
VBOOK = "/lib/书.txt"


def _wait_until(pred: Callable[[], bool], timeout: float = 15.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(0.05)
    return False


# --------------------------------------------------------------------- 配置/降级
def test_config_defaults(make_config) -> None:
    cfg = make_config()
    assert cfg.library.watch is True
    assert cfg.library.watch_debounce_s == pytest.approx(2.0)


def test_disabled_is_noop(config) -> None:
    config.library.watch = False
    watcher = LibraryWatcher(config, LibraryService(config))
    watcher.start()
    assert watcher.active is False
    assert watcher.status()["enabled"] is False
    watcher.stop()
    watcher.stop()  # 幂等


def test_degrades_without_watchfiles(make_config, monkeypatch) -> None:
    """未安装 watchfiles：start() 是安全的 no-op，不抛出、不建线程。"""
    monkeypatch.setattr(watcher_mod, "watch", None)
    cfg = make_config()
    watcher = LibraryWatcher(cfg, LibraryService(cfg))
    watcher.start()
    assert watcher.active is False
    status = watcher.status()
    assert status["available"] is False
    assert status["error"]


def test_no_watchable_root(make_config, tmp_path) -> None:
    cfg = make_config()
    cfg.library.roots = [
        LibraryRootCfg(name="gone", path=str(tmp_path / "nope"))
    ]
    watcher = LibraryWatcher(cfg, LibraryService(cfg))
    watcher.start()
    assert watcher.active is False
    watcher.stop()


# --------------------------------------------------------------------- 重扫调度
def test_unchanged_rescan_skips_index_write(make_config, tmp_books) -> None:
    """内容未变的重扫不得重写 index.json（review finding 2 的回归用例）。"""
    (tmp_books / "book.txt").write_text("内容", encoding="utf-8")
    library = LibraryService(make_config())
    assert library.scan_blocking() is True
    stat_after_first = library.config.index_path.stat()
    time.sleep(0.02)
    assert library.scan_blocking() is True  # 无变化：只走遍历，不落盘
    stat_after_second = library.config.index_path.stat()
    assert stat_after_second.st_mtime == pytest.approx(
        stat_after_first.st_mtime, abs=0.001
    )
    assert stat_after_second.st_size == stat_after_first.st_size


# --------------------------------------------------------------------- 真实监听
def test_watch_triggers_rescan(make_config, tmp_books) -> None:
    """新增 / 就地改写 / 删除都应自动触发增量重扫，改写还带内容变化通知。"""
    pytest.importorskip("watchfiles")
    cfg = make_config(library={"watch": True, "watch_debounce_s": 0.2})
    library = LibraryService(cfg)
    library.scan_blocking()
    assert library.book_count() == 0

    notified: List[str] = []
    library.add_content_listener(notified.append)
    watcher = LibraryWatcher(cfg, library, min_rescan_interval=0.4)
    watcher.start()
    try:
        assert watcher.active
        assert _wait_until(lambda: watcher.ready, timeout=5.0), "监听未进入事件循环"

        # 新增 → 入索引
        (tmp_books / "书.txt").write_text("第一版内容", encoding="utf-8")
        assert _wait_until(lambda: library.book_count() == 1), "新增文件未触发重扫"
        old_id = library.book_by_vpath(VBOOK).book_id
        assert old_id

        # 就地改写 → 索引切换到新 id，且旧 id 收到内容变化通知（缓存失效）
        time.sleep(0.4)  # 越过防抖窗口，让两批事件分开（合并也不影响正确性）
        (tmp_books / "书.txt").write_text("第二版内容，更长了", encoding="utf-8")
        assert _wait_until(lambda: bool(notified)), "内容变化未触发通知"
        assert old_id in notified
        assert library.book_by_vpath(VBOOK).book_id != old_id

        # 删除 → 出索引
        (tmp_books / "书.txt").unlink()
        assert _wait_until(lambda: library.book_count() == 0), "删除未触发重扫"
    finally:
        watcher.stop()
    assert not watcher.active


def test_burst_writes_converge_to_final_state(make_config, tmp_books) -> None:
    """快速连写（防抖窗口内多批事件）合并后，索引最终态必须等于磁盘文件。"""
    pytest.importorskip("watchfiles")
    cfg = make_config(library={"watch": True, "watch_debounce_s": 0.2})
    library = LibraryService(cfg)
    watcher = LibraryWatcher(cfg, library, min_rescan_interval=0.4)
    watcher.start()
    try:
        assert _wait_until(lambda: watcher.ready, timeout=5.0), "监听未进入事件循环"
        book = tmp_books / "burst.txt"
        for i in range(3):
            book.write_text(f"version {i} " + "x" * (i * 10), encoding="utf-8")
            time.sleep(0.05)
        final_size = book.stat().st_size

        def _converged() -> bool:
            # 必须以索引为准：book_by_vpath 在索引缺失时会退化为实时 stat，
            # 那样断言就是假阳性（review 期间实测踩过）
            if library.index_stats()["books"] != 1:
                return False
            ref = library.book_by_vpath("/lib/burst.txt")
            return (
                ref is not None
                and ref.size == final_size
                and library.book_by_id(ref.book_id) is not None
            )

        assert _wait_until(_converged), "连写后索引未收敛到最终状态"
        ref = library.book_by_vpath("/lib/burst.txt")
        assert ref.book_id == compute_book_id(book, final_size)
    finally:
        watcher.stop()
    assert not watcher.active
