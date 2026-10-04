"""跨实现（C++ server-cpp ↔ Python server/app）缓存与 layout_id 一致性。

按项目约定默认不运行；需要 Python 侧依赖（server/.venv 或 LNV_PYTHON）：

    cd server
    .venv\\Scripts\\python -m pytest ..\\server-cpp\\tests\\test_cross_impl.py -q

- Python 解释器：优先环境变量 ``LNV_PYTHON``，否则 ``..\\server\\.venv\\Scripts\\python.exe``；
- 解释器不存在或 app 包不可导入时**整文件 skip**（环境缺失不算失败）；
- (a) Python 写 parsed/layout 缓存 → C++ 服务器读取；C++ 写 layout 缓存 → Python 读取；
- (b) 同一固定 profile 下 C++ 的 layout_id == Python ``layout_key`` 计算结果。

本文件不启动 Python 版服务，只通过子进程调用 server/app 的库函数，
因此不会与 conftest 里的 C++ 服务 fixture 争端口/缓存目录。
"""

from __future__ import annotations

import gzip
import json
import os
import subprocess
from pathlib import Path

import pytest

CPP_ROOT = Path(__file__).resolve().parents[1]
SERVER_ROOT = CPP_ROOT.parent / "server"
DEFAULT_PY = SERVER_ROOT / ".venv" / "Scripts" / "python.exe"
PY = Path(os.environ.get("LNV_PYTHON") or DEFAULT_PY)

BOOK_TEXT = "第一章 跨实现\n\n" + "缓存互通测试。" * 40


def _flow_profile() -> dict:
    """与 Python LayoutProfile 校验一致的默认 flow 几何。"""
    return {
        "viewport": {"w": 600, "h": 800},
        "margin": {"left": 24, "right": 24, "top": 24, "bottom": 24},
        "font": {"family": "auto", "size_px": 32, "line_height": 1.6,
                 "letter_spacing_px": 0.0},
        "paragraph": {"spacing_px": 20.0, "first_line_indent_chars": 2},
        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
        "chapter_new_page": True,
        "mode": "flow",
        "strip_overlap_px": 0,
    }


def _run_py(script: str, *args: str) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env.setdefault("PYTHONIOENCODING", "utf-8")
    return subprocess.run(
        [str(PY), "-c", script, *map(str, args)],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
        timeout=120,
    )


@pytest.fixture(scope="module")
def py_ok() -> bool:
    if not PY.exists():
        pytest.skip(f"未找到 Python 解释器 {PY}（可用 LNV_PYTHON 指定）")
    probe = _run_py(
        "import sys; sys.path.insert(0, sys.argv[1]); import app; print('ok')",
        str(SERVER_ROOT),
    )
    if probe.returncode != 0:
        pytest.skip(f"server/app 包不可导入：{(probe.stderr or '')[-500:]}")
    return True


def _book_id(server, vdir: str, name: str) -> str:
    with server.client() as c:
        entries = c.get("/api/v1/files", params={"path": vdir}).json()["entries"]
    for e in entries:
        if e["name"] == name and e.get("book_id"):
            return e["book_id"]
    raise AssertionError(f"{name} 未出现在 {vdir} 的 /files 结果里")


def test_python_parsed_cache_read_by_cpp(server, make_book, py_ok):
    """Python 写 parsed 缓存 → C++ /layout 必须读取它（而不是重新解析）。"""
    make_book("cross/book.txt", BOOK_TEXT.encode("utf-8"))
    bid = _book_id(server, "/lib/cross", "book.txt")

    parsed_dir = server.cache / "parsed"
    parsed_dir.mkdir(parents=True, exist_ok=True)
    doc = {
        "book_id": bid,
        "format": "txt",
        "meta": {"title": "跨实现", "mode": "flow"},
        # 单章单块：无论 C++ 是否读取缓存，页数都应为 1；
        # 用独特章标题断言确实命中了 Python 写入的内容。
        "chapters": [
            {
                "index": 0,
                "title": "Python 写入的章节",
                "blocks": [
                    {"type": "paragraph",
                     "spans": [{"text": "缓存互通测试。"}]}
                ],
            }
        ],
    }
    with gzip.open(parsed_dir / f"{bid}.1.json.gz", "wt", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, separators=(",", ":"))

    with server.client() as c:
        r = c.post(f"/api/v1/books/{bid}/layout", json=_flow_profile())
    assert r.status_code == 200, r.text
    data = r.json()
    assert data["layout_id"].startswith("L")
    assert data["revision"] and len(data["revision"]) == 12
    assert data["total_pages"] >= 1
    # 章节标题来自 Python 写的 parsed 缓存 → C++ 确实读到了
    assert data["chapters"][0]["title"] == "Python 写入的章节"


def test_cpp_layout_cache_read_by_python(server, make_book, py_ok):
    """C++ 写 layout 缓存 → Python server/app 的 LayoutStore 数据类必须能解析。"""
    make_book("cross/layout.txt", BOOK_TEXT.encode("utf-8"))
    bid = _book_id(server, "/lib/cross", "layout.txt")
    with server.client() as c:
        r = c.post(f"/api/v1/books/{bid}/layout", json=_flow_profile())
    assert r.status_code == 200, r.text
    cpp = r.json()

    script = r"""
import glob, gzip, json, sys
layouts_dir, bid, server_root = sys.argv[1], sys.argv[2], sys.argv[3]
paths = glob.glob(layouts_dir + "/" + bid + "/*.json.gz")
assert paths, "C++ 未写出 layout 缓存"
with gzip.open(paths[0], "rt", encoding="utf-8") as fh:
    data = json.load(fh)
assert data["book_id"] == bid, data.get("book_id")
assert data["layout_id"] == sys.argv[4], (data["layout_id"], sys.argv[4])
sys.path.insert(0, server_root)
from app.layout.engine import PageMap          # noqa: E402
from app.models.schemas import LayoutProfile   # noqa: E402
pm = PageMap.from_dict(data["pagemap"])
LayoutProfile.model_validate(data["profile"])
print(pm.total_pages)
"""
    rr = _run_py(script, str(server.cache / "layouts"), bid, str(SERVER_ROOT),
                 cpp["layout_id"])
    assert rr.returncode == 0, rr.stderr
    assert int(rr.stdout.strip().splitlines()[-1]) == cpp["total_pages"]


def test_layout_id_matches_python(server, make_book, py_ok):
    """(b) 同一固定 profile：C++ /layout 的 layout_id == Python layout_key()。"""
    make_book("cross/lid.txt", "布局键一致性。".encode("utf-8"))
    bid = _book_id(server, "/lib/cross", "lid.txt")
    with server.client() as c:
        default_id = c.get("/api/v1/render/fonts").json()["default"]
    # family 用显式字体 id：两侧 resolve 都返回原串，layout_id 只取决于几何哈希；
    # 避免依赖系统字体发现问题导致跨机差异（字体发现差异不属于本测试目标）。
    profile = _flow_profile()
    profile["font"]["family"] = default_id
    with server.client() as c:
        r = c.post(f"/api/v1/books/{bid}/layout", json=profile)
    assert r.status_code == 200, r.text
    cpp_lid = r.json()["layout_id"]

    script = r"""
import json, sys
sys.path.insert(0, sys.argv[1])
from app.models.schemas import LayoutProfile
from app.services.layouts import layout_key

class _Fonts:  # 只要求 resolve_id：显式 family 原样返回
    def resolve_id(self, family):
        return family

profile = LayoutProfile.model_validate(json.loads(sys.argv[2]))
print(layout_key(_Fonts(), profile))
"""
    rr = _run_py(script, str(SERVER_ROOT), json.dumps(profile, ensure_ascii=False))
    assert rr.returncode == 0, rr.stderr
    assert cpp_lid == rr.stdout.strip().splitlines()[-1]


def test_revision_and_cover_wire_contract(server, make_book, py_ok):
    """revision / cover_url / 封面 ETag 与"删书后不得 304"契约。"""
    make_book("cross/wire.txt", BOOK_TEXT.encode("utf-8"))
    bid = _book_id(server, "/lib/cross", "wire.txt")

    with server.client() as c:
        meta = c.get(f"/api/v1/books/{bid}/meta")
        assert meta.status_code == 200, meta.text
        mj = meta.json()
        rev = mj["revision"]
        assert rev and len(rev) == 12
        assert mj["cover_url"] == f"/api/v1/books/{bid}/cover?rev={rev}"

        layout = c.post(f"/api/v1/books/{bid}/layout", json=_flow_profile())
        assert layout.json()["revision"] == rev  # 内容未变，两次请求 rev 相同

        etag = f'"{bid}:{rev}:w240"'
        cover = c.get(f"/api/v1/books/{bid}/cover", params={"w": 240},
                      headers={"If-None-Match": etag})
        assert cover.status_code in (200, 304)
        if cover.status_code == 200:
            assert cover.headers.get("etag") == etag
            cover2 = c.get(f"/api/v1/books/{bid}/cover", params={"w": 240},
                           headers={"If-None-Match": etag})
            assert cover2.status_code == 304

        # 删除文件后：封面必须先解析书 → 404，绝不允许凭 If-None-Match 命中 304
        (server.lib / "cross" / "wire.txt").unlink()
        gone = c.get(f"/api/v1/books/{bid}/cover", params={"w": 240},
                     headers={"If-None-Match": etag})
        assert gone.status_code == 404, gone.text
