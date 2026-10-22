"""书库搜索（`GET /library/search`，设计 5.2 #12b / 6.3）。

覆盖：书名/路径匹配、多关键词、排序优先级、截断、访问范围过滤、空库。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from app.services.access import AccessScope
from tests.conftest import png_bytes, write_cbz

API = "/api/v1"


def _write_txt(path: Path, title: str = "测试", chapters: int = 2) -> None:
    lines = [title]
    for i in range(chapters):
        for j in range(20):
            lines.append(f"第{i + 1}章第{j + 1}段正文，用于产生足够长度。")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


@pytest.fixture
def library(tmp_books: Path, context):
    _write_txt(tmp_books / "小说" / "三体.txt")
    _write_txt(tmp_books / "小说" / "三体II·黑暗森林.txt")
    _write_txt(tmp_books / "小说" / "活着.txt")
    _write_txt(tmp_books / "技术" / "三体问题与物理.txt")
    _write_txt(tmp_books / "随笔" / "读三体.txt")
    write_cbz(
        tmp_books / "漫画" / "三体画集.cbz",
        {f"p{i}.png": png_bytes(60, 90) for i in range(2)},
    )
    context.library.scan_blocking()
    return context


def _names(result) -> list[str]:
    return [entry["name"] for entry in result["entries"]]


# ------------------------------------------------------------------ 服务层
def test_search_matches_name_and_ranks_prefix_first(library):
    result = library.library.search_books("三体")
    assert result["total"] == 5  # 三体 / 三体II / 三体画集 / 三体问题 / 读三体
    names = _names(result)
    # 书名以关键词开头的排在"仅书名包含"的前面（读三体.txt 落在最后一名）
    assert names[0] == "三体.txt"
    assert set(names[:4]) == {"三体.txt", "三体II·黑暗森林.txt", "三体画集.cbz", "三体问题与物理.txt"}
    assert names[4] == "读三体.txt"


def test_search_is_case_insensitive_and_accepts_multiple_tokens(library):
    lowered = library.library.search_books("三体 ii")
    assert _names(lowered) == ["三体II·黑暗森林.txt"]
    # 多关键词 = 全部命中（空格分词，命中范围是整条虚拟路径）
    in_dir = library.library.search_books("小说 三体")
    assert set(_names(in_dir)) == {"三体.txt", "三体II·黑暗森林.txt"}


def test_search_matches_directory_part_of_path(library):
    result = library.library.search_books("漫画")
    assert _names(result) == ["三体画集.cbz"]


def test_search_multi_token_ranks_by_first_token(library, tmp_books: Path):
    """多关键词按**首个**关键词判断"书名开头"：整串几乎不可能是书名前缀。"""
    _write_txt(tmp_books / "随笔" / "黑暗森林与三体.txt")
    library.library.scan_blocking()

    names = _names(library.library.search_books("三体 黑暗"))
    assert names == ["三体II·黑暗森林.txt", "黑暗森林与三体.txt"]


def test_search_limit_and_truncated_flag(library):
    result = library.library.search_books("三体", limit=2)
    assert len(result["entries"]) == 2
    assert result["total"] == 5
    assert result["truncated"] is True

    wide = library.library.search_books("三体", limit=100)
    assert wide["truncated"] is False


def test_search_returns_book_entry_shape(library):
    entry = library.library.search_books("活着")["entries"][0]
    assert entry["type"] == "book"
    assert entry["format"] == "txt"
    assert entry["book_id"]
    assert entry["path"] == "/lib/小说/活着.txt"
    assert entry["has_contents"] is False
    assert entry["openable"] is True
    assert entry["cover_url"].startswith(f"/api/v1/books/{entry['book_id']}/cover?rev=")


def test_search_filters_by_access_scope(library):
    other = AccessScope(all=False, roots=frozenset({"another"}))
    assert library.library.search_books("三体", allowed=other)["total"] == 0

    own = AccessScope(all=False, roots=frozenset({"lib"}))
    assert library.library.search_books("三体", allowed=own)["total"] == 5


def test_search_blank_query_returns_nothing(library):
    assert library.library.search_books("   ")["total"] == 0
    assert library.library.search_books("")["entries"] == []


# -------------------------------------------------------------------- HTTP
def test_library_search_endpoint(config, tmp_books: Path):
    from fastapi.testclient import TestClient

    from app.main import create_app

    _write_txt(tmp_books / "小说" / "三体.txt")
    _write_txt(tmp_books / "小说" / "别的书.txt")

    app = create_app(config, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        resp = client.get(f"{API}/library/search", params={"q": "三体"})
        assert resp.status_code == 200
        body = resp.json()
        assert body["query"] == "三体"
        assert body["total"] == 1
        assert body["truncated"] is False
        entry = body["entries"][0]
        assert entry["name"] == "三体.txt"
        assert entry["book_id"]
        # 结果条目与 /files 的下发形状一致，客户端可复用同一套 DTO
        assert client.get(
            f"{API}/books/{entry['book_id']}/meta"
        ).status_code == 200


def test_library_search_endpoint_rejects_empty_query(config):
    from fastapi.testclient import TestClient

    from app.main import create_app

    app = create_app(config, background_scan=False)
    with TestClient(app) as client:
        assert client.get(f"{API}/library/search", params={"q": ""}).status_code in (400, 422)
        assert client.get(f"{API}/library/search").status_code in (400, 422)
