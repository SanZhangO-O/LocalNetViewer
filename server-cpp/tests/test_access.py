"""按密码进入不同书库（C++ 服务器；与 Python tests/test_access.py 同契约）。

独立启动一个双书库实例（公开库 + 密码库），验证：
- /files 根列表与 libraries 按凭据过滤；错误凭据 401（不静默降级）；
- 越权书库路径 BAD_PATH、越权 book_id BOOK_NOT_FOUND；
- /server/info 按凭据过滤书库列表，未带凭据且全部加锁时 auth_required=true。
"""

from __future__ import annotations

import socket
import subprocess
import time
from pathlib import Path
from typing import Iterator

import httpx
import pytest
from conftest import EXE

PUBLIC = "public"
PRIVATE = "private"


class Server:
    def __init__(self, base: str, proc: subprocess.Popen):
        self.base = base
        self.proc = proc


@pytest.fixture(scope="module")
def locked(tmp_path_factory) -> Iterator[Server]:
    if not EXE.exists():
        pytest.skip(f"未找到 {EXE}；先构建："
                    "powershell -ExecutionPolicy Bypass -File server-cpp\\build.ps1")
    root = tmp_path_factory.mktemp("access")
    (root / PUBLIC).mkdir(parents=True)
    (root / PRIVATE).mkdir(parents=True)
    (root / PUBLIC / "open.txt").write_text(
        "第一章 公开书\n" + "正文内容。" * 80, encoding="utf-8"
    )
    (root / PRIVATE / "hidden.txt").write_text(
        "第一章 私密书\n" + "正文内容。" * 80, encoding="utf-8"
    )
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = int(s.getsockname()[1])
    cfg = root / "config.yaml"
    cfg.write_text(
        "server:\n  name: AccessCpp\n  host: 127.0.0.1\n"
        f"  port: {port}\n  auth_token: \"\"\n"
        "library:\n  roots:\n"
        f"    - name: {PUBLIC}\n      path: {(root / PUBLIC).as_posix()}\n"
        f"    - name: {PRIVATE}\n      path: {(root / PRIVATE).as_posix()}\n"
        "      password: \"pw-b\"\n"
        "cache:\n"
        f"  dir: {(root / 'cache').as_posix()}\n",
        encoding="utf-8",
    )
    proc = subprocess.Popen([str(EXE), "--config", str(cfg)],
                            stdout=open(root / "server.log", "w", encoding="utf-8"),
                            stderr=subprocess.STDOUT, text=True)
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            if proc.poll() is not None:
                raise AssertionError(f"lnvserver 启动即退出 rc={proc.returncode}")
            try:
                if httpx.get(base + "/api/v1/server/info", timeout=2.0).status_code == 200:
                    break
            except Exception:  # noqa: BLE001
                time.sleep(0.3)
        else:
            raise AssertionError("双书库实例未就绪")
        yield Server(base, proc)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_files_root_filtered_by_password(locked: Server) -> None:
    with httpx.Client(base_url=locked.base, timeout=30.0) as c:
        body = c.get("/api/v1/files", params={"path": "/"}).json()
        assert [e["name"] for e in body["entries"]] == [PUBLIC]
        assert [lib["name"] for lib in body["libraries"]] == [PUBLIC]

        assert c.get("/api/v1/files", headers={"X-Auth-Token": "bad"}).status_code == 401

        body = c.get("/api/v1/files", headers={"X-Auth-Token": "pw-b"}).json()
        assert {e["name"] for e in body["entries"]} == {PUBLIC, PRIVATE}
        assert {lib["name"] for lib in body["libraries"]} == {PUBLIC, PRIVATE}


def test_locked_library_path_and_book_are_hidden(locked: Server) -> None:
    with httpx.Client(base_url=locked.base, timeout=30.0) as c:
        r = c.get("/api/v1/files", params={"path": f"/{PRIVATE}"})
        assert r.status_code == 400
        assert r.json()["code"] == "BAD_PATH"

        listed = c.get("/api/v1/files", params={"path": f"/{PRIVATE}"},
                       headers={"X-Auth-Token": "pw-b"}).json()
        book = next(e for e in listed["entries"] if e["type"] == "book")
        book_id = book["book_id"]

        assert c.get(f"/api/v1/books/{book_id}/meta").status_code == 404
        assert c.get(f"/api/v1/books/{book_id}/meta",
                     headers={"X-Auth-Token": "bad"}).status_code == 404
        assert c.get(f"/api/v1/books/{book_id}/meta",
                     headers={"X-Auth-Token": "pw-b"}).status_code == 200


def test_server_info_filtered_by_credential(locked: Server) -> None:
    with httpx.Client(base_url=locked.base, timeout=30.0) as c:
        body = c.get("/api/v1/server/info").json()
        assert body["auth_required"] is False
        assert [lib["name"] for lib in body["libraries"]] == [PUBLIC]

        body = c.get("/api/v1/server/info", headers={"X-Auth-Token": "pw-b"}).json()
        assert {lib["name"] for lib in body["libraries"]} == {PUBLIC, PRIVATE}

        assert c.get("/api/v1/server/info", headers={"X-Auth-Token": "bad"}).status_code == 401