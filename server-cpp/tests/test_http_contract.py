"""HTTP 契约回归测试：鉴权、错误信封、路径安全、Range/304、整数边界。

对应审查里"HTTP 层"的修复项；客户端按 code 分支，所以这些必须锁住。
"""

from __future__ import annotations

import io
import zipfile

import httpx
import pytest
from conftest import png_bytes

FLOW = {
    "viewport": {"w": 1080, "h": 1920},
    "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "flow",
}


@pytest.fixture(scope="module")
def txt_book(server):
    """一本稳定的 TXT（供契约类用例复用）。"""
    path = server.lib / "http" / "book.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("第1章 契约测试\n" + "正文内容。" * 500, encoding="utf-8")
    return server.book_id("/lib/http", "book.txt")


def test_error_envelope_shape(server, txt_book):
    """错误体固定是 {code, message[, detail]}，客户端按 code 分支。"""
    with server.client() as c:
        r = c.get(f"/api/v1/books/{txt_book}/pages/1", params={"layout_id": "Lbogus"})
        assert r.status_code == 404
        body = r.json()
        assert body["code"] == "LAYOUT_NOT_FOUND"
        assert body["detail"]["layout_id"] == "Lbogus"
        assert set(body) <= {"code", "message", "detail"}


def test_page_number_is_not_truncated(server, txt_book):
    """页码参数是无界 int：4294967297 必须 404 并回显原值（审查 H4）。

    旧实现 static_cast<int> 后 4294967297 → 1，于是"越界页"返回了第 1 页的图片。
    """
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{txt_book}/layout", json=FLOW).json()
        r = c.get(f"/api/v1/books/{txt_book}/pages/4294967297",
                  params={"layout_id": lay["layout_id"]})
        assert r.status_code == 404
        assert r.json()["detail"]["page"] == 4294967297


def test_files_offset_is_not_truncated(server, txt_book):
    """offset 巨大时返回空列表并回显原值（旧实现截断成 0 → 返回整页列表）。"""
    with server.client() as c:
        r = c.get("/api/v1/files", params={"path": "/lib/http", "offset": "4294967296"})
        assert r.status_code == 200
        body = r.json()
        assert body["entries"] == []
        assert body["offset"] == 4294967296


def test_range_beyond_int64_is_handled(server):
    """Range 值超过 int64 时按"超出文件"处理（416 / 206），不是退化成 200（审查 M5）。"""
    path = server.lib / "http" / "rng.cbz"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("01.png", png_bytes(20, 30))
    bid = server.book_id("/lib/http", "rng.cbz")
    fixed = dict(FLOW, mode="fixed", margin={"left": 0, "right": 0, "top": 0, "bottom": 0})
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{bid}/layout", json=fixed).json()
        base = {"layout_id": lay["layout_id"], "fit": "original"}
        total = len(c.get(f"/api/v1/books/{bid}/pages/1", params=base).content)

        r = c.get(f"/api/v1/books/{bid}/pages/1", params=base,
                  headers={"Range": "bytes=99999999999999999999-"})
        assert r.status_code == 416
        assert r.headers["content-range"] == f"bytes */{total}"

        r = c.get(f"/api/v1/books/{bid}/pages/1", params=base,
                  headers={"Range": "bytes=-99999999999999999999"})
        assert r.status_code == 206
        assert r.headers["content-range"] == f"bytes 0-{total - 1}/{total}"

        r = c.get(f"/api/v1/books/{bid}/pages/1", params=base,
                  headers={"Range": "bytes=abc"})
        assert r.status_code == 200, "语法非法的 Range 必须忽略（RFC 9110 + Python 行为）"


def test_etag_and_304(server, txt_book):
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{txt_book}/layout", json=FLOW).json()
        url = f"/api/v1/books/{txt_book}/pages/1"
        r1 = c.get(url, params={"layout_id": lay["layout_id"]})
        etag = r1.headers["etag"]
        assert r1.headers["cache-control"].startswith("private")
        r2 = c.get(url, params={"layout_id": lay["layout_id"]}, headers={"If-None-Match": etag})
        assert r2.status_code == 304


def test_path_traversal_matrix(server):
    """`..`/编码穿越/反斜杠/盘符/NUL/UNC/保留名一律 400 且不泄露真实路径。"""
    bad = ["/../..", "/%2e%2e%2f", "\\..\\..", "C:/Windows", "/lib/../../etc",
           "/lib/a\0b", "//server/share", "/lib/nul"]
    with server.client() as c:
        for p in bad:
            r = c.get("/api/v1/files", params={"path": p})
            assert r.status_code in (400, 403), f"{p!r} -> {r.status_code}"
            assert r.json()["code"] in ("BAD_PATH", "PATH_TRAVERSAL")


def test_head_unknown_path_is_404(server):
    """未知路径的 HEAD 与 Python 一致是 404（已知路径 405）。"""
    with server.client() as c:
        assert c.request("HEAD", "/api/v1/nope").status_code == 404
        assert c.request("HEAD", "/healthz").status_code == 405


def test_trailing_slash_redirects(server):
    with server.client() as c:
        r = c.get("/api/v1/files/", follow_redirects=False)
        assert r.status_code == 307
        assert r.headers["location"].endswith("/api/v1/files")


def test_oversized_uri_returns_json_envelope(server):
    """httplib 自身的 414 也要带 JSON 错误体，否则客户端解析失败（审查 M10）。"""
    with server.client() as c:
        r = c.get("/api/v1/files", params={"path": "/" + "x" * 9000})
        assert r.status_code == 414
        assert r.json()["code"] == "BAD_REQUEST"


def test_query_validation_matches_pydantic(server):
    """宽松整数的边界：'1e3'/'0x10' 非法、'10.0' 合法、'1_0' 合法（审查 M4）。"""
    with server.client() as c:
        for v, expect in (("1e3", 400), ("0x10", 400), ("10.5", 400),
                          ("10.0", 200), ("1_0", 200), ("+10", 200)):
            r = c.get("/api/v1/files", params={"path": "/lib", "limit": v})
            assert r.status_code == expect, f"limit={v!r} -> {r.status_code}"


def test_open_folder_non_image_detail(server):
    path = server.lib / "http" / "plain.txt"
    path.write_text("x", encoding="utf-8")
    with server.client() as c:
        r = c.post("/api/v1/books/open-folder", json={"path": "/lib/http/plain.txt"})
        assert r.status_code == 415
        assert r.json()["detail"]["reason"] == "非图片文件"


def test_open_folder_recursive_must_be_bool(server):
    with server.client() as c:
        r = c.post("/api/v1/books/open-folder",
                   json={"path": "/lib/http", "recursive": "bogus"})
        assert r.status_code == 400


def test_fit_original_is_cached(server):
    """fit=original 的整页字节也要进内存缓存（旧实现每次重新解码/编码，审查 M11）。"""
    path = server.lib / "http" / "cache.cbz"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("01.png", png_bytes(120, 180))
    bid = server.book_id("/lib/http", "cache.cbz")
    fixed = dict(FLOW, mode="fixed", margin={"left": 0, "right": 0, "top": 0, "bottom": 0})
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{bid}/layout", json=fixed).json()
        url = f"/api/v1/books/{bid}/pages/1"
        params = {"layout_id": lay["layout_id"], "fit": "original"}
        first = c.get(url, params=params)
        second = c.get(url, params=params)
        assert first.status_code == second.status_code == 200
        assert first.content == second.content
        # 同一 ETag + 相同字节：第二次必然走缓存（无法直接观测，但至少不能变）
        assert first.headers["etag"] == second.headers["etag"]


def test_auth_matrix(tmp_path, server):
    """有令牌时：业务路由 401+WWW-Authenticate，/server/info 豁免（独立实例）。"""
    import socket
    import subprocess
    import time
    from conftest import EXE

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = int(s.getsockname()[1])
    cfg = tmp_path / "auth.yaml"
    cfg.write_text(
        "server:\n  name: Auth\n  host: 127.0.0.1\n"
        f"  port: {port}\n  auth_token: \"s3cr3t\"\n"
        "library:\n  roots:\n    - name: lib\n"
        f"      path: {server.lib.as_posix()}\n"
        "cache:\n"
        f"  dir: {(tmp_path / 'cache').as_posix()}\n",
        encoding="utf-8",
    )
    proc = subprocess.Popen([str(EXE), "--config", str(cfg)],
                            stdout=open(tmp_path / "auth.log", "w", encoding="utf-8"),
                            stderr=subprocess.STDOUT, text=True)
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                if httpx.get(base + "/api/v1/server/info", timeout=2.0).status_code == 200:
                    break
            except Exception:  # noqa: BLE001
                time.sleep(0.3)
        else:
            raise AssertionError("带令牌的实例未就绪")
        with httpx.Client(base_url=base, timeout=30.0) as c:
            r = c.get("/api/v1/files", params={"path": "/lib"})
            assert r.status_code == 401
            assert r.headers["www-authenticate"] == "X-Auth-Token"
            assert r.json()["detail"]["auth_required"] is True
            assert c.get("/api/v1/server/info").status_code == 200
            assert c.get("/api/v1/files", params={"path": "/lib"},
                         headers={"X-Auth-Token": "s3cr3t"}).status_code == 200
            assert c.get("/api/v1/files", params={"path": "/lib"},
                         headers={"X-Auth-Token": "wrong"}).status_code == 401
            assert c.get("/docs").status_code == 404
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
