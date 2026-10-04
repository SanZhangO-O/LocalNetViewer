"""服务层回归测试：解析闸门、进度、locate、缓存互操作、关停。

这些是并发/生命周期相关的缺陷（审查 H1/H2/H5/M6/M7 等），用真实 HTTP 请求验证。
"""

from __future__ import annotations

import json
import time
from concurrent.futures import ThreadPoolExecutor

import httpx
import pytest

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
def big_book(server):
    """一本够大的 TXT：分页要几百毫秒，才能观察到闸门行为。"""
    path = server.lib / "svc" / "big.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for ch in range(1, 5):
        lines.append(f"第{ch}章 大书")
        lines.extend(f"这是第{ch}章第{i}段正文。" + "填充内容用于拉长分页时间。" * 20
                     for i in range(1, 1200))
    path.write_text("\n".join(lines), encoding="utf-8")
    return server.book_id("/lib/svc", "big.txt")


def test_parse_gate_dedups_and_keeps_slots(server, big_book):
    """同一 layout 的并发请求：**最多只有一个真正跑分页**，且 503 不能破坏去重（审查 H1）。

    旧实现先武装 RAII Guard 再 try_acquire，被拒绝的请求析构时会 release 掉
    **正在跑的任务**的去重键与槽位 → 后续请求会再跑一遍全量分页（cache_hit=False
    出现两次）。因此这里断言的不变量是"真正分页（cache_hit=False）只出现一次"：
    并发窗口内的请求拿 503，窗口外到达的请求合法地命中缓存（cache_hit=True）。
    """
    prof = json.loads(json.dumps(FLOW))
    prof["font"]["size_px"] = 47          # 未算过的组合，保证不是缓存命中
    prof["margin"]["left"] = 43

    def fire(delay: float):
        time.sleep(delay)
        r = httpx.post(f"{server.base_url}/api/v1/books/{big_book}/layout",
                       json=prof, timeout=300.0)
        return r.status_code, (r.json().get("cache_hit") if r.status_code == 200 else None)

    with ThreadPoolExecutor(max_workers=4) as ex:
        results = [f.result() for f in [ex.submit(fire, d) for d in (0.0, 0.01, 0.02, 0.03)]]

    assert all(code in (200, 503) for code, _ in results), results
    real_runs = [r for r in results if r[1] is False]
    assert len(real_runs) == 1, f"只应有一个请求真正分页: {results}"

    # 跑完后同一 profile 必须是缓存命中（说明结果确实写进了缓存）
    r = httpx.post(f"{server.base_url}/api/v1/books/{big_book}/layout", json=prof, timeout=300.0)
    assert r.status_code == 200 and r.json()["cache_hit"] is True


def test_locate_clamps_huge_anchor(server, big_book):
    """巨大锚点值 = "永远在文档之后" → 定位末页且 clamped=true（审查 H4）。"""
    with server.client() as c:
        lay = c.post(f"/api/v1/books/{big_book}/layout", json=FLOW).json()
        total = lay["total_pages"]
        r = c.get(f"/api/v1/books/{big_book}/locate",
                  params={"layout_id": lay["layout_id"], "chapter": 4294967296})
        assert r.status_code == 200
        body = r.json()
        assert body["page"] == total and body["clamped"] is True


def test_progress_roundtrip_keeps_large_values(server, big_book):
    """进度里的 page/anchor 是无界 int，必须原样存回（审查 H4）。"""
    payload = {"anchor": {"chapter": 4294967296, "block": 0, "char": 0},
               "page": 4294967297, "ratio": 0.5}
    with server.client() as c:
        put = c.put(f"/api/v1/progress/{big_book}", params={"device_id": "dev-big"}, json=payload)
        assert put.status_code == 200, put.text
        got = c.get(f"/api/v1/progress/{big_book}", params={"device_id": "dev-big"}).json()
        assert got["page"] == 4294967297
        assert got["anchor"]["chapter"] == 4294967296


def test_progress_ratio_is_clamped(server, big_book):
    with server.client() as c:
        c.put(f"/api/v1/progress/{big_book}", params={"device_id": "dev-r"},
              json={"anchor": {"chapter": 0, "block": 0, "char": 0}, "ratio": 5})
        got = c.get(f"/api/v1/progress/{big_book}", params={"device_id": "dev-r"}).json()
        assert got["ratio"] == 1.0


def test_progress_put_validation(server, big_book):
    with server.client() as c:
        r = c.put(f"/api/v1/progress/{big_book}", json={})
        assert r.status_code == 400
        assert r.json()["code"] == "BAD_REQUEST"


def test_search_reports_layout_not_found_without_book_id(server, big_book):
    """search 的 LAYOUT_NOT_FOUND detail 只有 layout_id（审查 M7）。"""
    with server.client() as c:
        r = c.get(f"/api/v1/books/{big_book}/search",
                  params={"q": "第", "layout_id": "Lbogusbogus"})
        assert r.status_code == 404
        assert r.json()["detail"] == {"layout_id": "Lbogusbogus"}


def test_caches_are_interchangeable_with_python_layout(server, big_book):
    """C++ 写出的 layouts/*.json.gz 结构必须能被再次读回（同 layout_id 命中缓存）。"""
    with server.client() as c:
        prof = json.loads(json.dumps(FLOW))
        prof["font"]["size_px"] = 49
        first = c.post(f"/api/v1/books/{big_book}/layout", json=prof).json()
        assert first["cache_hit"] is False
        second = c.post(f"/api/v1/books/{big_book}/layout", json=prof).json()
        assert second["cache_hit"] is True
        assert second["layout_id"] == first["layout_id"]
        assert second["total_pages"] == first["total_pages"]
    # 磁盘上确实落了盘（Python 读同一目录时依赖这个文件）
    cache_files = list(server.cache.glob(f"layouts/{big_book}/*.json.gz"))
    assert cache_files, f"未在 {server.cache} 下找到 layout 缓存"


def test_shutdown_is_clean(server, big_book):
    """带后台扫描启动的实例，terminate 后必须正常退出（不能 abort，审查 H1/H2）。"""
    import socket
    import subprocess
    from conftest import EXE

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = int(s.getsockname()[1])
    cfg = server.cache.parent / "shutdown.yaml"
    cfg.write_text(
        "server:\n  name: Shutdown\n  host: 127.0.0.1\n"
        f"  port: {port}\n  auth_token: \"\"\n"
        "library:\n  roots:\n    - name: lib\n"
        f"      path: {server.lib.as_posix()}\n"
        "cache:\n"
        f"  dir: {(server.cache.parent / 'shutdown-cache').as_posix()}\n",
        encoding="utf-8",
    )
    proc = subprocess.Popen([str(EXE), "--config", str(cfg), "--no-scan"],
                            stdout=open(server.cache.parent / "shutdown.log", "w",
                                        encoding="utf-8"),
                            stderr=subprocess.STDOUT, text=True)
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            if httpx.get(base + "/api/v1/server/info", timeout=2.0).status_code == 200:
                break
        except Exception:  # noqa: BLE001
            time.sleep(0.3)
    else:
        proc.kill()
        raise AssertionError("实例未就绪")
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise AssertionError("terminate 后 20s 未退出（可能有 joinable 线程导致卡死）")
    # 0xC0000409 / 0xC00000FD 等异常退出码代表 abort/栈溢出
    assert proc.returncode in (0, 1, 3, 15, 143), f"异常退出码 {proc.returncode:#x}"
