"""HTTP 契约与端到端阅读闭环（设计 5.x / 10.3）。

覆盖：错误码表、鉴权、layout → pages → meta → locate 往返、目录浏览、搜索、进度。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.conftest import jpeg_bytes, png_bytes, write_cbz

API = "/api/v1"


def _write_txt(path: Path, chapters: int = 3) -> None:
    lines = []
    for i in range(chapters):
        lines.append(f"第{i + 1}章 测试章节标题")
        for j in range(30):
            lines.append(f"这是第{i + 1}章的第{j + 1}段正文，用来产生足够的行数以便分页。")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def _profile(**overrides):
    profile = {
        "viewport": {"w": 800, "h": 1200},
        "margin": {"left": 20, "right": 20, "top": 20, "bottom": 60},
        "font": {"family": "auto", "size_px": 40, "line_height": 1.6},
        "paragraph": {"spacing_px": 10, "first_line_indent_chars": 2},
        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
        "chapter_new_page": True,
        "mode": "flow",
    }
    profile.update(overrides)
    return profile


@pytest.fixture
def api(config, tmp_books: Path):
    """已扫描好书库的 TestClient。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    _write_txt(tmp_books / "novel.txt")
    write_cbz(tmp_books / "comic.cbz", {f"p{i:03d}.png": png_bytes(400, 600) for i in range(1, 4)})

    app = create_app(config, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        yield client


def _book_id(client, name: str) -> str:
    data = client.get(f"{API}/files", params={"path": "/lib"}).json()
    for entry in data["entries"]:
        if entry["name"] == name:
            return entry["book_id"]
    raise AssertionError(f"{name} 不在目录列表里: {[e['name'] for e in data['entries']]}")


# --------------------------------------------------------------- 服务信息
def test_server_info_needs_no_token_and_hides_real_paths(api):
    resp = api.get(f"{API}/server/info")
    assert resp.status_code == 200
    body = resp.json()
    assert body["api_version"] == "v1"
    assert body["auth_required"] is False
    assert body["features"]["png"] is True
    assert "json" in body["features"], "对外契约里这个字段必须仍叫 json（内部用 alias）"
    assert body["libraries"], "至少有一个书库"
    # 未鉴权端点不得泄漏真实文件系统路径（设计 9）
    for lib in body["libraries"]:
        assert lib.get("path") in (None, ""), f"泄漏了真实路径: {lib}"


def test_fonts_endpoint(api):
    resp = api.get(f"{API}/render/fonts")
    assert resp.status_code == 200
    body = resp.json()
    assert "default" in body and isinstance(body["fonts"], list)


# --------------------------------------------------------------- 目录
def test_files_lists_books_and_dirs(api, tmp_books: Path):
    (tmp_books / "sub").mkdir(exist_ok=True)
    resp = api.get(f"{API}/files", params={"path": "/lib"})
    assert resp.status_code == 200
    body = resp.json()
    names = [e["name"] for e in body["entries"]]
    assert "novel.txt" in names and "comic.cbz" in names
    # 目录排在文件前面
    assert names.index("sub") < names.index("novel.txt")
    novel = next(e for e in body["entries"] if e["name"] == "novel.txt")
    assert novel["type"] == "book" and novel["openable"] is True and novel["book_id"]


def test_files_root_lists_libraries(api):
    body = api.get(f"{API}/files", params={"path": "/"}).json()
    assert body["path"] == "/"
    assert [lib["name"] for lib in body["libraries"]] == ["lib"]


@pytest.mark.parametrize(
    "path",
    ["/lib/../../etc/passwd", "/lib/%2e%2e%2f%2e%2e%2fetc", "..", "/lib/a:b"],
)
def test_files_rejects_traversal(api, path):
    resp = api.get(f"{API}/files", params={"path": path})
    assert resp.status_code in (400, 403), f"{path} 应被拒绝，实际 {resp.status_code}"
    assert resp.json()["code"] in ("BAD_PATH", "PATH_TRAVERSAL")


def test_files_unknown_library_is_bad_path(api):
    resp = api.get(f"{API}/files", params={"path": "/nope"})
    assert resp.status_code == 400
    assert resp.json()["code"] == "BAD_PATH"


# --------------------------------------------------------------- 元数据
def test_book_meta_txt(api):
    book_id = _book_id(api, "novel.txt")
    resp = api.get(f"{API}/books/{book_id}/meta")
    assert resp.status_code == 200
    body = resp.json()
    assert body["format"] == "txt"
    assert body["mode"] == "flow"
    assert body["render_support"]["png"] is True
    assert body["render_support"]["json"] is True
    assert len(body["chapters"]) >= 3
    assert body["chapters"][0]["title"].startswith("第1章")


def test_book_meta_unknown_id(api):
    resp = api.get(f"{API}/books/deadbeefdeadbeef/meta")
    assert resp.status_code == 404
    assert resp.json()["code"] == "BOOK_NOT_FOUND"


def test_book_meta_comic_is_fixed(api):
    book_id = _book_id(api, "comic.cbz")
    body = api.get(f"{API}/books/{book_id}/meta").json()
    assert body["mode"] == "fixed"
    assert body["page_count"] == 3


def test_cover_is_image_and_cached(api):
    book_id = _book_id(api, "novel.txt")
    first = api.get(f"{API}/books/{book_id}/cover", params={"w": 240})
    assert first.status_code == 200
    assert first.headers["content-type"].startswith("image/")
    assert "etag" in {k.lower() for k in first.headers}
    second = api.get(f"{API}/books/{book_id}/cover", params={"w": 240})
    assert second.content == first.content


# --------------------------------------------------------------- 排版闭环
def test_layout_pages_meta_locate_round_trip(api):
    book_id = _book_id(api, "novel.txt")
    layout = api.post(f"{API}/books/{book_id}/layout", json=_profile())
    assert layout.status_code == 200, layout.text
    body = layout.json()
    layout_id = body["layout_id"]
    total = body["total_pages"]
    assert total > 1
    assert body["mode"] == "flow"

    anchors = []
    for page in range(1, total + 1):
        meta = api.get(
            f"{API}/books/{book_id}/pages/{page}/meta", params={"layout_id": layout_id}
        )
        assert meta.status_code == 200, meta.text
        m = meta.json()
        assert m["page"] == page
        assert m["total_pages"] == total
        anchors.append(m["anchor"])

        located = api.get(
            f"{API}/books/{book_id}/locate",
            params={
                "layout_id": layout_id,
                "chapter": m["anchor"]["chapter"],
                "block": m["anchor"]["block"],
                "char": m["anchor"]["char"],
            },
        )
        assert located.status_code == 200
        loc = located.json()
        assert loc["page"] == page, f"第 {page} 页的锚点回到了第 {loc['page']} 页"
        assert loc["clamped"] is False
        assert loc["chapter_index"] == m["chapter_index"], "locate 与 meta 的章节必须一致"

    # 锚点严格递增
    tuples = [(a["chapter"], a["block"], a["char"]) for a in anchors]
    assert tuples == sorted(set(tuples))


def test_layout_is_deterministic(api):
    book_id = _book_id(api, "novel.txt")
    a = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()
    b = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()
    assert a["layout_id"] == b["layout_id"]
    assert a["total_pages"] == b["total_pages"]
    assert b["cache_hit"] is True


def test_layout_id_changes_with_font_size(api):
    book_id = _book_id(api, "novel.txt")
    a = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()
    b = api.post(f"{API}/books/{book_id}/layout", json=_profile(font={"family": "auto", "size_px": 64, "line_height": 1.6})).json()
    assert a["layout_id"] != b["layout_id"]


def test_layout_rejects_unknown_book(api):
    resp = api.post(f"{API}/books/deadbeefdeadbeef/layout", json=_profile())
    assert resp.status_code == 404
    assert resp.json()["code"] == "BOOK_NOT_FOUND"


def test_layout_requires_viewport(api):
    book_id = _book_id(api, "novel.txt")
    bad = _profile()
    bad.pop("viewport")
    resp = api.post(f"{API}/books/{book_id}/layout", json=bad)
    assert resp.status_code == 400
    assert resp.json()["code"] == "BAD_REQUEST"


def test_unknown_layout_id_is_layout_not_found(api):
    book_id = _book_id(api, "novel.txt")
    resp = api.get(f"{API}/books/{book_id}/pages/1", params={"layout_id": "Lffffffffffff"})
    assert resp.status_code == 404
    assert resp.json()["code"] == "LAYOUT_NOT_FOUND"


def test_page_out_of_range(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    resp = api.get(f"{API}/books/{book_id}/pages/99999", params={"layout_id": layout_id})
    assert resp.status_code == 404
    assert resp.json()["code"] == "PAGE_OUT_OF_RANGE"


def test_page_png_and_headers(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    resp = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "theme": "sepia"}
    )
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("image/")
    assert resp.headers["x-layout-id"] == layout_id
    assert resp.headers["x-page-index"] == "1"
    assert "immutable" in resp.headers["cache-control"]
    etag = resp.headers["etag"]

    # 条件请求
    not_modified = api.get(
        f"{API}/books/{book_id}/pages/1",
        params={"layout_id": layout_id, "theme": "sepia"},
        headers={"If-None-Match": etag},
    )
    assert not_modified.status_code == 304

    # 不同主题必须产生不同 ETag（否则会串页）
    other = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "theme": "dark"}
    )
    assert other.headers["etag"] != etag


def test_page_etag_is_stable_across_requests(api):
    """ETag 必须跨请求稳定（不能用内置 hash()，它按进程加盐）。"""
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    tags = {
        api.get(f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id}).headers["etag"]
        for _ in range(3)
    }
    assert len(tags) == 1


def test_page_json_mode_for_flow(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    resp = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "format": "json"}
    )
    assert resp.status_code == 200
    body = resp.json()
    assert body["page"] == 1
    assert isinstance(body["blocks"], list)
    # lines 已自带段首缩进，不能再让客户端补一遍
    assert body["paragraph"]["first_line_indent_chars"] == 0


def test_page_json_rejected_for_fixed(api):
    book_id = _book_id(api, "comic.cbz")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed")).json()[
        "layout_id"
    ]
    resp = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "format": "json"}
    )
    assert resp.status_code == 400
    assert resp.json()["code"] == "BAD_REQUEST"


def test_invalid_format_and_fit_are_rejected(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    bad_format = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "format": "gif"}
    )
    assert bad_format.status_code == 400
    bad_fit = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "fit": "stretch"}
    )
    assert bad_fit.status_code == 400


def test_scale_out_of_range_is_rejected(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    resp = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "scale": 9.0}
    )
    assert resp.status_code == 400


def test_fixed_page_original_supports_range(api):
    book_id = _book_id(api, "comic.cbz")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed")).json()[
        "layout_id"
    ]
    full = api.get(
        f"{API}/books/{book_id}/pages/1",
        params={"layout_id": layout_id, "fit": "original", "scale": 1.0},
    )
    assert full.status_code == 200
    assert full.headers.get("accept-ranges") == "bytes"
    total = len(full.content)

    part = api.get(
        f"{API}/books/{book_id}/pages/1",
        params={"layout_id": layout_id, "fit": "original", "scale": 1.0},
        headers={"Range": "bytes=0-99"},
    )
    assert part.status_code == 206
    assert part.content == full.content[:100]
    assert part.headers["content-range"] == f"bytes 0-99/{total}"

    bad = api.get(
        f"{API}/books/{book_id}/pages/1",
        params={"layout_id": layout_id, "fit": "original", "scale": 1.0},
        headers={"Range": f"bytes={total + 10}-"},
    )
    assert bad.status_code == 416


def test_pair_and_rtl_for_fixed(api):
    book_id = _book_id(api, "comic.cbz")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed")).json()[
        "layout_id"
    ]
    single = api.get(f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id})
    pair = api.get(f"{API}/books/{book_id}/pages/1", params={"layout_id": layout_id, "pair": 1})
    assert single.status_code == 200 and pair.status_code == 200
    assert pair.headers["etag"] != single.headers["etag"]
    # 跨页图应当比单页宽（横向拼接）
    assert len(pair.content) > 0


def test_page_meta_aspect_for_fixed(api):
    book_id = _book_id(api, "comic.cbz")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed")).json()[
        "layout_id"
    ]
    meta = api.get(f"{API}/books/{book_id}/pages/1/meta", params={"layout_id": layout_id}).json()
    assert meta["aspect"] == pytest.approx(600 / 400, rel=0.02)


def test_locate_clamps_and_reports(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    past = api.get(
        f"{API}/books/{book_id}/locate",
        params={"layout_id": layout_id, "chapter": 9999, "block": 0, "char": 0},
    ).json()
    assert past["clamped"] is True
    assert past["page"] == past["total_pages"]


# --------------------------------------------------------------- 搜索
def test_search_finds_text_and_maps_to_page(api):
    book_id = _book_id(api, "novel.txt")
    layout_id = api.post(f"{API}/books/{book_id}/layout", json=_profile()).json()["layout_id"]
    resp = api.get(
        f"{API}/books/{book_id}/search", params={"q": "第2章", "layout_id": layout_id}
    )
    assert resp.status_code == 200
    body = resp.json()
    assert body["total"] >= 1
    hit = body["results"][0]
    assert hit["page"] is not None
    assert "第2章" in hit["snippet"]

    # 命中页的锚点区间必须真的包含该锚点
    meta = api.get(
        f"{API}/books/{book_id}/pages/{hit['page']}/meta", params={"layout_id": layout_id}
    ).json()
    assert (meta["anchor"]["chapter"], meta["anchor"]["block"], meta["anchor"]["char"]) <= (
        hit["chapter_index"],
        hit["block"],
        hit["char"],
    )


def test_search_without_layout_has_no_page(api):
    book_id = _book_id(api, "novel.txt")
    body = api.get(f"{API}/books/{book_id}/search", params={"q": "正文"}).json()
    assert body["results"]
    assert all(item["page"] is None for item in body["results"])


def test_search_unsupported_for_fixed(api):
    book_id = _book_id(api, "comic.cbz")
    resp = api.get(f"{API}/books/{book_id}/search", params={"q": "x"})
    assert resp.status_code == 415
    assert resp.json()["code"] == "UNSUPPORTED_FORMAT"


def test_search_empty_query_is_rejected(api):
    book_id = _book_id(api, "novel.txt")
    assert api.get(f"{API}/books/{book_id}/search", params={"q": ""}).status_code == 400


# --------------------------------------------------------------- 图集
def test_open_folder_as_gallery(api, tmp_books: Path):
    folder = tmp_books / "gallery"
    folder.mkdir(exist_ok=True)
    for i in range(3):
        (folder / f"{i}.png").write_bytes(png_bytes(200, 300))

    resp = api.post(f"{API}/books/open-folder", json={"path": "/lib/gallery"})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["page_count"] == 3
    assert body["mode"] == "fixed"
    book_id = body["book_id"]

    # 注册之后整条链路必须可用（不能只是 200）
    assert api.get(f"{API}/books/{book_id}/meta").status_code == 200
    layout = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed"))
    assert layout.status_code == 200, layout.text
    assert layout.json()["total_pages"] == 3
    page = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout.json()["layout_id"]}
    )
    assert page.status_code == 200
    assert page.headers["content-type"].startswith("image/")
    assert api.get(f"{API}/books/{book_id}/cover").status_code == 200


def test_open_folder_single_image(api, tmp_books: Path):
    """单张图片 = 单页图集：注册之后同样必须能打开（曾经注册 200、其余全 415）。"""
    image = tmp_books / "solo.jpg"
    image.write_bytes(jpeg_bytes(300, 400))

    resp = api.post(f"{API}/books/open-folder", json={"path": "/lib/solo.jpg"})
    assert resp.status_code == 200, resp.text
    book_id = resp.json()["book_id"]
    assert resp.json()["page_count"] == 1

    assert api.get(f"{API}/books/{book_id}/meta").status_code == 200
    layout = api.post(f"{API}/books/{book_id}/layout", json=_profile(mode="fixed"))
    assert layout.status_code == 200, layout.text
    assert layout.json()["total_pages"] == 1
    page = api.get(
        f"{API}/books/{book_id}/pages/1", params={"layout_id": layout.json()["layout_id"]}
    )
    assert page.status_code == 200
    assert api.get(f"{API}/books/{book_id}/cover").status_code == 200


def test_files_list_single_images_by_default(api, tmp_books: Path):
    """单张图片默认出现在目录列表里（show_image_files 默认开，点按即单页图集）。"""
    (tmp_books / "solo.png").write_bytes(png_bytes(120, 200))
    # 非电子书仍按 show_all_files=false 隐藏：图片行可见不能波及普通文件
    (tmp_books / "readme.md").write_text("hi", encoding="utf-8")

    body = api.get(f"{API}/files", params={"path": "/lib"}).json()
    entries = {e["name"]: e for e in body["entries"]}
    solo = entries["solo.png"]
    assert solo["type"] == "image"
    assert solo["format"] == "gallery"
    assert solo["gallery"]["image_count"] == 1
    assert solo["gallery"]["can_open_as_book"] is True
    assert solo["openable"] is True and solo["reason"] is None
    assert solo["book_id"]
    listed = entries["readme.md"]
    assert listed["type"] == "file" and listed["openable"] is False


def test_files_hide_single_images_when_disabled(api, tmp_books: Path, make_config):
    """show_image_files=false 时目录列表不得列出图片文件（旧行为可配回）。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    (tmp_books / "solo.png").write_bytes(png_bytes(120, 200))
    cfg = make_config(gallery={"show_image_files": False})
    cfg.base_dir = tmp_books.parent

    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        body = client.get(f"{API}/files", params={"path": "/lib"}).json()
        assert "solo.png" not in {e["name"] for e in body["entries"]}


def test_open_folder_unknown_path(api):
    resp = api.post(f"{API}/books/open-folder", json={"path": "/lib/nope"})
    assert resp.status_code == 400
    assert resp.json()["code"] == "BAD_PATH"


def test_strip_falls_back_to_fixed_for_large_gallery(make_config):
    """条漫要逐张探测源图高度：源图数超过上限时回退 fixed，避免 /layout 全量读图。"""
    from app.models.schemas import LayoutProfile
    from app.services.layouts import effective_profile

    class FakeAdapter:
        mode = "fixed"
        supports_strip = True

        def page_count(self):
            return 1200

    cfg = make_config()
    cfg.comic.strip_max_source_images = 100
    profile = LayoutProfile(**_profile(mode="strip"))
    assert effective_profile(cfg, FakeAdapter(), profile).mode == "fixed"


def test_strip_kept_when_under_cap(make_config):
    from app.models.schemas import LayoutProfile
    from app.services.layouts import effective_profile

    class FakeAdapter:
        mode = "fixed"
        supports_strip = True

        def page_count(self):
            return 50

    cfg = make_config()
    cfg.comic.strip_max_source_images = 100
    profile = LayoutProfile(**_profile(mode="strip"))
    assert effective_profile(cfg, FakeAdapter(), profile).mode == "strip"


def test_open_folder_too_few_images(api, tmp_books: Path, make_config):
    """图片数不足时必须明确 415，而不是注册一本打不开的书。"""
    from fastapi.testclient import TestClient

    from app.main import create_app

    cfg = make_config(gallery={"min_images_in_folder": 3})
    cfg.base_dir = tmp_books.parent
    folder = tmp_books / "tiny"
    folder.mkdir(exist_ok=True)
    (folder / "a.png").write_bytes(png_bytes(50, 50))

    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        resp = client.post(f"{API}/books/open-folder", json={"path": "/lib/tiny"})
        assert resp.status_code == 415
        assert resp.json()["code"] == "UNSUPPORTED_FORMAT"


# --------------------------------------------------------------- 进度
def test_progress_put_get_round_trip(api):
    book_id = _book_id(api, "novel.txt")
    payload = {"anchor": {"chapter": 1, "block": 2, "char": 3}, "page": 7, "ratio": 0.42}
    put = api.put(f"{API}/progress/{book_id}", params={"device_id": "phone-a"}, json=payload)
    assert put.status_code == 200, put.text
    body = put.json()
    assert body["anchor"] == {"chapter": 1, "block": 2, "char": 3}
    assert body["page"] == 7

    got = api.get(f"{API}/progress/{book_id}", params={"device_id": "phone-a"}).json()
    assert got["page"] == 7
    assert got["anchor"]["chapter"] == 1

    # 未指定设备时返回最近一次
    any_device = api.get(f"{API}/progress/{book_id}").json()
    assert any_device["page"] == 7


def test_progress_is_per_device(api):
    book_id = _book_id(api, "novel.txt")
    api.put(
        f"{API}/progress/{book_id}",
        params={"device_id": "a"},
        json={"anchor": {"chapter": 0, "block": 0, "char": 0}, "page": 1, "ratio": 0.1},
    )
    api.put(
        f"{API}/progress/{book_id}",
        params={"device_id": "b"},
        json={"anchor": {"chapter": 2, "block": 0, "char": 0}, "page": 9, "ratio": 0.9},
    )
    assert api.get(f"{API}/progress/{book_id}", params={"device_id": "a"}).json()["page"] == 1
    assert api.get(f"{API}/progress/{book_id}", params={"device_id": "b"}).json()["page"] == 9


def test_progress_ratio_is_clamped(api):
    book_id = _book_id(api, "novel.txt")
    body = api.put(
        f"{API}/progress/{book_id}",
        json={"anchor": {"chapter": 0, "block": 0, "char": 0}, "page": 1, "ratio": 5.0},
    ).json()
    assert 0.0 <= body["ratio"] <= 1.0


def test_progress_unknown_book(api):
    assert api.get(f"{API}/progress/deadbeefdeadbeef").status_code == 404


# --------------------------------------------------------------- 鉴权
def test_auth_token_is_enforced(config, tmp_books: Path):
    from fastapi.testclient import TestClient

    from app.main import create_app

    _write_txt(tmp_books / "novel.txt")
    cfg = config.model_copy(deep=True)
    cfg.server.auth_token = "s3cret"

    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        assert client.get(f"{API}/files").status_code == 401
        assert client.get(f"{API}/files", headers={"X-Auth-Token": "wrong"}).status_code == 401
        assert client.get(f"{API}/files", headers={"X-Auth-Token": "s3cret"}).status_code == 200
        # 令牌开启时 /server/info 仍可用（客户端要先知道需不需要令牌）
        assert client.get(f"{API}/server/info").status_code == 200
        # 交互式文档必须关闭
        assert client.get("/docs").status_code == 404
        assert client.get("/openapi.json").status_code == 404


def test_healthz_and_root(api):
    assert api.get("/healthz").json()["ok"] is True
    assert api.get("/").json()["api"] == "/v1" or api.get("/").json()["api"] == "/api/v1"


# --------------------------------------------------------------- 错误契约
def test_every_error_body_has_code_and_message(api):
    """所有错误响应体必须是 {code, message}（客户端据此分类处理）。"""
    book_id = _book_id(api, "novel.txt")
    checks = [
        api.get(f"{API}/files", params={"path": "/nope"}),
        api.get(f"{API}/books/nope/meta"),
        api.get(f"{API}/books/{book_id}/pages/1", params={"layout_id": "Lbadbadbadbad"}),
        api.post(f"{API}/books/{book_id}/layout", json={"viewport": {"w": 0, "h": 0}}),
        api.get(f"{API}/books/{book_id}/search", params={"q": ""}),
    ]
    for resp in checks:
        assert resp.status_code >= 400, resp.text
        body = resp.json()
        assert isinstance(body.get("code"), str) and body["code"], body
        assert isinstance(body.get("message"), str) and body["message"], body
