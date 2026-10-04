"""server-cpp 回归测试夹具：临时书库 + 临时缓存 + 真实启动 lnvserver.exe。

运行（用 server 的虚拟环境，里面已有 pytest/httpx/Pillow）：
    cd server
    .venv\\Scripts\\python -m pytest ..\\server-cpp\\tests -q

设计要点：
- 每个测试会话用自己的临时书库/缓存目录，不依赖仓库里的 .e2e 或 config.yaml；
- 端口自动挑空闲端口，测试之间不互相干扰；
- 没有构建产物时整体 skip 并给出构建提示（不让 CI 里出现看不懂的失败）。
"""

from __future__ import annotations

import io
import socket
import subprocess
import time
from pathlib import Path
from typing import Iterator

import httpx
import pytest
from PIL import Image

CPP_ROOT = Path(__file__).resolve().parents[1]
EXE = CPP_ROOT / "build" / "lnvserver.exe"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def png_bytes(width: int, height: int, color=(200, 30, 30)) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (max(1, width), max(1, height)), color).save(buf, format="PNG")
    return buf.getvalue()


class Server:
    def __init__(self, base_url: str, proc: subprocess.Popen, lib: Path, cache: Path):
        self.base_url = base_url
        self.proc = proc
        self.lib = lib
        self.cache = cache

    def client(self, **kw) -> httpx.Client:
        kw.setdefault("timeout", 120.0)
        return httpx.Client(base_url=self.base_url, **kw)

    def book_id(self, vpath_dir: str, name: str) -> str:
        with self.client() as c:
            entries = c.get("/api/v1/files", params={"path": vpath_dir}).json()["entries"]
        for e in entries:
            if e["name"] == name and e.get("book_id"):
                return e["book_id"]
        raise AssertionError(f"{name} 未出现在 {vpath_dir} 的列表里")


@pytest.fixture(scope="session")
def lib_root(tmp_path_factory) -> Path:
    root = tmp_path_factory.mktemp("library") / "books"
    root.mkdir(parents=True)
    return root


@pytest.fixture(scope="session")
def server(lib_root: Path, tmp_path_factory) -> Iterator[Server]:
    if not EXE.exists():
        pytest.skip(f"未找到 {EXE}；先构建："
                    "powershell -ExecutionPolicy Bypass -File server-cpp\\build.ps1")
    cache = tmp_path_factory.mktemp("cache")
    port = free_port()
    cfg_dir = tmp_path_factory.mktemp("cfg")
    cfg = cfg_dir / "config.yaml"
    log_path = cfg_dir / "server.log"
    cfg.write_text(
        "server:\n"
        "  name: CppTests\n"
        "  host: 127.0.0.1\n"
        f"  port: {port}\n"
        '  auth_token: ""\n'
        "library:\n"
        "  roots:\n"
        "    - name: lib\n"
        f"      path: {lib_root.as_posix()}\n"
        "  include_ext: [\".txt\", \".epub\", \".pdf\", \".cbz\", \".cbr\", \".cb7\",\n"
        "                \".cbt\", \".fb2\", \".mobi\", \".azw3\", \".zip\", \".rar\",\n"
        "                \".7z\", \".tar\"]\n"
        "limits:\n"
        "  max_archive_entry_mb: 16\n"   # 小上限：压缩包守卫用例不必造 512MB 数据
        "cache:\n"
        f"  dir: {cache.as_posix()}\n",
        encoding="utf-8",
    )
    proc = subprocess.Popen(
        [str(EXE), "--config", str(cfg)],
        # 必须重定向到文件而不是 PIPE：服务端每个请求都打一行访问日志，
        # PIPE 没人读会在 64KB 处写满，子进程阻塞在 printf 上 → 整个测试挂死。
        stdout=open(log_path, "w", encoding="utf-8"),
        stderr=subprocess.STDOUT,
        text=True,
    )
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            raise AssertionError(
                f"lnvserver 启动即退出 rc={proc.returncode}\n"
                f"{log_path.read_text(encoding='utf-8', errors='replace')[-2000:]}"
            )
        try:
            r = httpx.get(base + "/api/v1/server/info", timeout=2.0)
            if r.status_code == 200:
                break
        except Exception:  # noqa: BLE001
            time.sleep(0.3)
    else:
        proc.kill()
        raise AssertionError("lnvserver 未在 60s 内就绪")
    yield Server(base, proc, lib_root, cache)
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()


@pytest.fixture
def make_book(lib_root: Path):
    def _make(relpath: str, data: bytes) -> Path:
        p = lib_root / relpath
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    return _make
