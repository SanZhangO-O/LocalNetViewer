"""按密码进入不同书库（设计 9）的回归测试。

覆盖：
- access.resolve_access 的凭据解析规则（管理员令牌 / 书库密码 / 公开 / 缺失）；
- HTTP 层：根目录与 libraries 列表过滤、越权路径与书籍 404、/server/info 按凭据过滤。
"""

from __future__ import annotations

from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import (
    AppConfig,
    CacheCfg,
    ComicCfg,
    GalleryCfg,
    LibraryCfg,
    LibraryRootCfg,
    LimitsCfg,
    RenderCfg,
    ServerCfg,
)
from app.main import create_app
from app.services.access import resolve_access

API = "/api/v1"

PUBLIC = "公开库"
PRIVATE = "私密库"


def _make_config(tmp_path: Path, *, auth_token: str = "") -> AppConfig:
    public_dir = tmp_path / "public"
    private_dir = tmp_path / "private"
    public_dir.mkdir(parents=True, exist_ok=True)
    private_dir.mkdir(parents=True, exist_ok=True)
    cfg = AppConfig(
        server=ServerCfg(name="AccessTest", auth_token=auth_token),
        library=LibraryCfg(
            roots=[
                LibraryRootCfg(name=PUBLIC, path=str(public_dir)),
                LibraryRootCfg(name=PRIVATE, path=str(private_dir), password="pw-b"),
            ]
        ),
        gallery=GalleryCfg(),
        comic=ComicCfg(),
        render=RenderCfg(fonts_dir=str(tmp_path / "fonts")),
        cache=CacheCfg(dir=str(tmp_path / "cache")),
        limits=LimitsCfg(),
    )
    cfg.base_dir = tmp_path
    return cfg


def _write_txt(path: Path, title: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"第一章 {title}\n" + "正文内容。" * 80, encoding="utf-8")


def _client(tmp_path: Path, *, auth_token: str = "") -> TestClient:
    cfg = _make_config(tmp_path, auth_token=auth_token)
    app = create_app(cfg, background_scan=False)
    client = TestClient(app)
    client.__enter__()
    app.state.ctx.library.scan_blocking()
    return client


# ------------------------------------------------------------ 解析规则（单元）
def test_resolve_access_admin_token_sees_all(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path, auth_token="admin-secret")
    result = resolve_access(cfg, "admin-secret")
    assert result.valid and result.scope.all


def test_resolve_access_root_password_sees_match_plus_public(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path)
    result = resolve_access(cfg, "pw-b")
    assert result.valid
    assert not result.scope.all
    assert result.scope.allows_root(PRIVATE)
    assert result.scope.allows_root(PUBLIC)
    assert not result.scope.allows_root("不存在的库")


def test_resolve_access_wrong_token_is_invalid(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path)
    result = resolve_access(cfg, "nope")
    assert not result.valid and not result.missing


def test_resolve_access_no_token_public_only(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path)
    result = resolve_access(cfg, None)
    assert result.valid
    assert result.scope.allows_root(PUBLIC)
    assert not result.scope.allows_root(PRIVATE)


def test_resolve_access_missing_when_all_locked(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path)
    cfg.library.roots[0].password = "pw-a"
    result = resolve_access(cfg, "")
    assert not result.valid and result.missing
    # 输入任一书库密码即可进入（另有公开库时同样包含公开库）
    assert resolve_access(cfg, "pw-a").scope.allows_root(PUBLIC)
    assert resolve_access(cfg, "pw-b").scope.allows_root(PRIVATE)


# ------------------------------------------------------------ HTTP 行为（集成）
@pytest.fixture
def api(tmp_path: Path):
    client = _client(tmp_path)
    _write_txt(tmp_path / "public" / "open.txt", "公开书")
    _write_txt(tmp_path / "private" / "hidden.txt", "私密书")
    yield client
    client.__exit__(None, None, None)


def test_files_root_filtered_by_password(api: TestClient) -> None:
    # 无密码：根目录只见公开库
    resp = api.get(f"{API}/files", params={"path": "/"})
    assert resp.status_code == 200
    body = resp.json()
    names = [e["name"] for e in body["entries"]]
    assert names == [PUBLIC]
    assert [lib["name"] for lib in body["libraries"]] == [PUBLIC]

    # 错误密码：401（不静默降级为"只看到公开库"）
    assert api.get(f"{API}/files", headers={"X-Auth-Token": "bad"}).status_code == 401

    # 书库密码：公开 + 对应书库
    resp = api.get(f"{API}/files", headers={"X-Auth-Token": "pw-b"})
    assert resp.status_code == 200
    body = resp.json()
    assert {e["name"] for e in body["entries"]} == {PUBLIC, PRIVATE}
    assert {lib["name"] for lib in body["libraries"]} == {PUBLIC, PRIVATE}


def test_locked_library_path_and_book_are_hidden(api: TestClient) -> None:
    # 未授权书库与不存在书库同错误码（不泄露存在性）
    resp = api.get(f"{API}/files", params={"path": f"/{PRIVATE}"})
    assert resp.status_code == 400
    assert resp.json()["code"] == "BAD_PATH"

    # 拿正确密码拿到 book_id 后，无密码客户端仍不可访问其内容
    listed = api.get(
        f"{API}/files", params={"path": f"/{PRIVATE}"}, headers={"X-Auth-Token": "pw-b"}
    ).json()
    book = next(e for e in listed["entries"] if e["type"] == "book")
    book_id = book["book_id"]

    assert api.get(f"{API}/books/{book_id}/meta").status_code == 404
    # 错误令牌在任何端点都是统一的 401（与 /files 的错误密码 401 同一契约，
    # 不因路径/书籍不同而变化，同样不泄露存在性）
    assert (
        api.get(f"{API}/books/{book_id}/meta", headers={"X-Auth-Token": "bad"}).status_code
        == 401
    )
    assert (
        api.get(f"{API}/books/{book_id}/meta", headers={"X-Auth-Token": "pw-b"}).status_code
        == 200
    )


def test_server_info_filtered_by_credential(api: TestClient) -> None:
    resp = api.get(f"{API}/server/info")
    assert resp.status_code == 200
    body = resp.json()
    assert body["auth_required"] is False
    assert [lib["name"] for lib in body["libraries"]] == [PUBLIC]

    resp = api.get(f"{API}/server/info", headers={"X-Auth-Token": "pw-b"})
    assert resp.status_code == 200
    assert {lib["name"] for lib in resp.json()["libraries"]} == {PUBLIC, PRIVATE}

    assert api.get(f"{API}/server/info", headers={"X-Auth-Token": "bad"}).status_code == 401


def test_all_locked_requires_password(tmp_path: Path) -> None:
    cfg = _make_config(tmp_path)
    cfg.library.roots[0].password = "pw-a"
    app = create_app(cfg, background_scan=False)
    with TestClient(app) as client:
        app.state.ctx.library.scan_blocking()
        assert client.get(f"{API}/files").status_code == 401
        info = client.get(f"{API}/server/info")
        assert info.status_code == 200
        assert info.json()["auth_required"] is True
        assert info.json()["libraries"] == []
        ok = client.get(f"{API}/files", headers={"X-Auth-Token": "pw-a"})
        assert ok.status_code == 200
        # 全部书库加锁后没有"公开书库"可叠加：pw-a 只可见它对应的那一书库
        # （与 resolve_access 单元测试 test_resolve_access_missing_when_all_locked 一致）
        assert {e["name"] for e in ok.json()["entries"]} == {PUBLIC}


def test_admin_token_still_sees_locked_libraries(tmp_path: Path) -> None:
    client = _client(tmp_path, auth_token="admin-secret")
    try:
        resp = client.get(f"{API}/files", headers={"X-Auth-Token": "admin-secret"})
        assert resp.status_code == 200
        assert {e["name"] for e in resp.json()["entries"]} == {PUBLIC, PRIVATE}
        # 管理员令牌启用后，书库密码仍可进入对应书库（不是唯一凭据）
        scoped = client.get(f"{API}/files", headers={"X-Auth-Token": "pw-b"})
        assert scoped.status_code == 200
        assert {e["name"] for e in scoped.json()["entries"]} == {PUBLIC, PRIVATE}
    finally:
        client.__exit__(None, None, None)
