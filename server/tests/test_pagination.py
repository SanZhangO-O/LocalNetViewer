"""书库目录分页（设计 5.2 #2 / 5.3）。

两个关注点：

1. 契约：``limit/offset`` 切页稳定 —— 全量翻完不重不漏、顺序（目录优先 +
   自然排序）跨页一致、越界 offset 返回空页但 total 仍正确；
2. 成本：分页必须裁剪**服务器每页的工作量**，而不只是响应体 —— 重活
   （book_id 指纹 = 每文件读 1MB 头）只允许发生在当前页的条目上，
   否则目录里有几万个文件时，翻到第 2 页也要把整个目录重新指纹一遍。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from app.services import library as library_mod
from app.services.library import LibraryService
from app.util.natural import natural_key

API = "/api/v1"
ROOT = "/lib"


def _fill_books(books: Path, count: int, prefix: str = "书") -> None:
    for i in range(count):
        (books / f"{prefix}_{i:04d}.txt").write_text("内容" * 20, encoding="utf-8")


def _walk_all(svc: LibraryService, page_size: int) -> tuple[list[str], int]:
    """像客户端一样按 offset 滚动翻完整个目录，返回 (名字序列, total)。"""
    names: list[str] = []
    offset = 0
    total = -1
    while True:
        page = svc.list_dir(ROOT, limit=page_size, offset=offset)
        total = int(page["total"])
        entries = page["entries"]
        if not entries:
            break
        names.extend(e["name"] for e in entries)
        offset += len(entries)
        if offset >= total:
            break
    return names, total


# --------------------------------------------------------------------- 契约
def test_pages_cover_every_entry_without_gaps_or_duplicates(config, tmp_books):
    _fill_books(tmp_books, 57)
    (tmp_books / "子目录").mkdir()
    (tmp_books / "子目录" / "被跳过的书.txt").write_text("x", encoding="utf-8")

    svc = LibraryService(config)
    names, total = _walk_all(svc, page_size=10)

    assert total == 58  # 57 本书 + 1 个子目录（子目录里的书不计入本目录）
    assert len(names) == len(set(names)) == 58
    # 目录优先，其余自然排序；跨页拼接后仍整体有序
    assert names[0] == "子目录"
    file_names = names[1:]
    assert file_names == sorted(file_names, key=natural_key)


def test_offset_beyond_total_returns_empty_page(config, tmp_books):
    _fill_books(tmp_books, 3)
    svc = LibraryService(config)

    page = svc.list_dir(ROOT, limit=10, offset=100)

    assert page["entries"] == []
    assert page["total"] == 3
    assert page["offset"] == 100
    assert page["limit"] == 10


def test_subdir_gallery_info_present_on_any_page(config, tmp_books):
    """"打开为图集"标记跟着目录条目走：目录被切到后面的页时标记不能丢。"""
    for i in range(12):
        (tmp_books / f"book_{i:04d}.txt").write_text("内容" * 20, encoding="utf-8")
    for i in range(10):  # 填充目录：让图集目录排到第 2 页（目录组整体在文件组前）
        (tmp_books / f"dir_{i:02d}").mkdir()
    gallery_dir = tmp_books / "z_gallery"
    gallery_dir.mkdir()
    for i in range(3):
        (gallery_dir / f"{i}.png").write_bytes(b"fake")

    svc = LibraryService(config)
    # 列表顺序：dir_00..dir_09（0-9）、z_gallery（10）、book_*（11+）
    page = svc.list_dir(ROOT, limit=5, offset=10)

    dirs = [e for e in page["entries"] if e["type"] == "dir"]
    assert [d["name"] for d in dirs] == ["z_gallery"]
    assert dirs[0]["gallery"]["image_count"] == 3
    assert dirs[0]["gallery"]["can_open_as_book"] is True


# --------------------------------------------------------------------- 成本
def test_page_request_only_fingerprints_page_entries(config, tmp_books, monkeypatch):
    """无索引的大目录：一次取页只允许指纹当前页条目（回归：旧实现全目录读头）。"""
    _fill_books(tmp_books, 40)
    svc = LibraryService(config)  # 不扫描：40 个文件全部处于"未指纹"状态

    real_compute = library_mod.compute_book_id
    fingerprinted: list[str] = []

    def spy(path, size=None):
        fingerprinted.append(Path(path).name)
        return real_compute(path, size)

    monkeypatch.setattr(library_mod, "compute_book_id", spy)

    page = svc.list_dir(ROOT, limit=5, offset=10)

    assert len(page["entries"]) == 5
    assert all(e["book_id"] for e in page["entries"])
    # 旧实现这里会把 40 个文件全部读 1MB 头算指纹；分页后只对页内 5 个付成本
    assert len(fingerprinted) == 5
    assert {e["name"] for e in page["entries"]} == set(fingerprinted)


def test_second_page_reuses_fingerprints_from_first(config, tmp_books, monkeypatch):
    """同一文件的指纹计算一次后进索引，后续翻页不得重复读文件头。"""
    _fill_books(tmp_books, 8)
    svc = LibraryService(config)

    real_compute = library_mod.compute_book_id
    calls = {"n": 0}

    def spy(path, size=None):
        calls["n"] += 1
        return real_compute(path, size)

    monkeypatch.setattr(library_mod, "compute_book_id", spy)

    first = svc.list_dir(ROOT, limit=4, offset=0)
    second = svc.list_dir(ROOT, limit=4, offset=4)

    assert len(first["entries"]) == 4 and len(second["entries"]) == 4
    assert calls["n"] == 8  # 8 个文件各算一次，页与页之间命中索引缓存


# --------------------------------------------------------------------- HTTP 层
def test_files_api_pagination_contract(client, tmp_books):
    """/files 的 total/limit/offset 契约：客户端据此滚动加载。"""
    _fill_books(tmp_books, 12)

    first = client.get(API + "/files", params={"path": ROOT, "limit": 5, "offset": 0})
    assert first.status_code == 200
    body = first.json()
    assert body["total"] == 12
    assert body["limit"] == 5 and body["offset"] == 0
    assert len(body["entries"]) == 5

    second = client.get(API + "/files", params={"path": ROOT, "limit": 5, "offset": 5})
    assert second.status_code == 200
    body2 = second.json()

    names1 = {e["name"] for e in body["entries"]}
    names2 = {e["name"] for e in body2["entries"]}
    assert not (names1 & names2)  # 页与页不重叠
    assert all(e["book_id"] for e in body2["entries"])  # 翻页条目同样可打开


def test_files_api_rejects_invalid_pagination(client, tmp_books):
    _fill_books(tmp_books, 2)
    # 校验错误统一映射为 400 BAD_REQUEST（errors.py 的 RequestValidationError 处理器）
    for params in ({"limit": 0}, {"offset": -1}, {"limit": 5001}):
        resp = client.get(API + "/files", params={"path": ROOT, **params})
        assert resp.status_code == 400, params
        assert resp.json()["code"] == "BAD_REQUEST"


@pytest.mark.parametrize("page_size", [1, 7, 1000])
def test_pagination_matches_full_listing(page_size, config, tmp_books):
    """任意页大小的分页结果都必须与一次性全量列表完全一致。"""
    _fill_books(tmp_books, 23)
    (tmp_books / "笔记.txt.bak").write_text("x", encoding="utf-8")  # 未含扩展名过滤
    svc = LibraryService(config)

    full = [e["name"] for e in svc.list_dir(ROOT, limit=1000, offset=0)["entries"]]
    paged, total = _walk_all(svc, page_size=page_size)

    assert total == len(full)
    assert paged == full
