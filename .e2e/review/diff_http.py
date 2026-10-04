"""C++ 移植版 vs Python 参照版的 HTTP 对拍（review 用，不属于发布内容）。

用法（两个服务器需先各自启动，见同目录 py-config.yaml / cpp-config.yaml）：
    .venv\\Scripts\\python.exe -X utf8 ..\\..\\.e2e\\review\\diff_http.py

比对维度：HTTP 状态、Content-Type、契约相关响应头、JSON 响应体（递归比较）、
错误体（code/message/detail）。图片字节只比 mime 与非空（编码器不同，字节必然不同）。

退出码：0 = 无差异；1 = 存在差异。
"""

from __future__ import annotations

import json
import re
import sys
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import httpx

PY = "http://127.0.0.1:8901"
CPP = "http://127.0.0.1:8902"
LIB = Path(__file__).resolve().parents[1] / "library"

# 契约相关的响应头（值会逐字比较；volatile 的不在其中）
HEADERS = [
    "content-type",
    "etag",
    "cache-control",
    "x-page-index",
    "x-total-pages",
    "x-layout-id",
    "accept-ranges",
    "content-range",
    "www-authenticate",
    "retry-after",
    "content-disposition",
]

# JSON 里随请求/时间变化的键
VOLATILE = {"server_time", "updated_at"}

PROFILES = {
    "flow": {
        "viewport": {"w": 1080, "h": 1920},
        "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
        "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
        "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
        "chapter_new_page": True,
        "mode": "flow",
    },
    "flow_big": {
        "viewport": {"w": 1080, "h": 1920},
        "margin": {"left": 60, "right": 60, "top": 60, "bottom": 120},
        "font": {"family": "auto", "size_px": 88, "line_height": 2.0},
        "paragraph": {"spacing_px": 30, "first_line_indent_chars": 0},
        "image": {"max_width_ratio": 0.8, "max_height_ratio": 0.5},
        "chapter_new_page": False,
        "mode": "flow",
    },
    "fixed": {
        "viewport": {"w": 1080, "h": 1920},
        "margin": {"left": 0, "right": 0, "top": 0, "bottom": 0},
        "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
        "paragraph": {"spacing_px": 0, "first_line_indent_chars": 0},
        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
        "chapter_new_page": True,
        "mode": "fixed",
    },
    "strip": {
        "viewport": {"w": 1080, "h": 1920},
        "margin": {"left": 0, "right": 0, "top": 0, "bottom": 0},
        "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
        "paragraph": {"spacing_px": 0, "first_line_indent_chars": 0},
        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
        "chapter_new_page": True,
        "mode": "strip",
        "strip_overlap_px": 40,
    },
}

diffs: List[str] = []
checks = 0
notes: List[str] = []


def note(msg: str) -> None:
    notes.append(msg)


def record(name: str, detail: str) -> None:
    diffs.append(f"{name}: {detail}")


def norm(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: ("<volatile>" if k in VOLATILE else norm(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [norm(v) for v in value]
    return value


def parse_body(resp: httpx.Response) -> Any:
    ctype = resp.headers.get("content-type", "")
    if "json" in ctype:
        try:
            return norm(resp.json())
        except Exception as exc:  # noqa: BLE001
            return f"<unparsable json: {exc}: {resp.text[:200]!r}>"
    return None


def is_image(ctype: str) -> bool:
    return ctype.startswith("image/") or ctype.startswith("font/")


def compare(name: str, a: httpx.Response, b: httpx.Response) -> None:
    """a = Python 参照，b = C++ 移植。"""
    global checks
    checks += 1

    if a.status_code != b.status_code:
        record(name, f"status {a.status_code} (py) vs {b.status_code} (cpp)")

    for h in HEADERS:
        va, vb = a.headers.get(h), b.headers.get(h)
        if va != vb:
            record(name, f"header {h}: {va!r} (py) vs {vb!r} (cpp)")

    ca = a.headers.get("content-type", "")
    cb = b.headers.get("content-type", "")

    if is_image(ca) and is_image(cb):
        # 图片/字体：编码器不同，只比是否为空、mime 与量级
        if not a.content or not b.content:
            record(name, f"empty image body: py={len(a.content)} cpp={len(b.content)}")
        elif ca.split(";")[0] != cb.split(";")[0]:
            record(name, f"image mime: {ca} (py) vs {cb} (cpp)")
        else:
            ratio = len(b.content) / max(1, len(a.content))
            if ratio < 0.5 or ratio > 2.0:
                record(name, f"image size ratio {ratio:.2f} (py={len(a.content)} cpp={len(b.content)})")
        return

    if "json" in ca or "json" in cb:
        ba, bb = parse_body(a), parse_body(b)
        if ba != bb:
            record(name, f"json body differs:\n    py  = {json.dumps(ba, ensure_ascii=False, sort_keys=True)[:900]}\n    cpp = {json.dumps(bb, ensure_ascii=False, sort_keys=True)[:900]}")
        return

    if a.content != b.content:
        record(name, f"body differs (py {len(a.content)}B vs cpp {len(b.content)}B): "
                     f"py={a.text[:200]!r} cpp={b.text[:200]!r}")


def get_pair(py: httpx.Client, cpp: httpx.Client, name: str, path: str,
             params: Optional[dict] = None, headers: Optional[dict] = None) -> Tuple[httpx.Response, httpx.Response]:
    a = py.get(path, params=params, headers=headers)
    b = cpp.get(path, params=params, headers=headers)
    compare(name, a, b)
    return a, b


def post_pair(py: httpx.Client, cpp: httpx.Client, name: str, path: str,
              body: Any = None, params: Optional[dict] = None,
              content: Optional[bytes] = None) -> Tuple[httpx.Response, httpx.Response]:
    kw: Dict[str, Any] = {"params": params}
    if content is not None:
        kw["content"] = content
        kw["headers"] = {"Content-Type": "application/json"}
    else:
        kw["json"] = body
    a = py.post(path, **kw)
    b = cpp.post(path, **kw)
    compare(name, a, b)
    return a, b


def walk(client: httpx.Client, path: str = "/", depth: int = 0) -> List[dict]:
    """递归收集所有可打开的条目（用 Python 服务器的视图做基准）。"""
    out: List[dict] = []
    if depth > 4:
        return out
    data = client.get("/api/v1/files", params={"path": path}).json()
    for e in data.get("entries", []):
        if e.get("type") == "dir":
            out.extend(walk(client, e["path"], depth + 1))
        elif e.get("book_id"):
            out.append(e)
    return out


def main() -> int:
    with httpx.Client(timeout=120.0, base_url=PY) as py, \
            httpx.Client(timeout=120.0, base_url=CPP) as cpp:
        # ---------------------------------------------------------- 基本信息
        get_pair(py, cpp, "server/info", "/api/v1/server/info")
        get_pair(py, cpp, "root", "/")
        get_pair(py, cpp, "healthz", "/healthz")

        # ---------------------------------------------------------- 字体
        fa = py.get("/api/v1/render/fonts").json()
        fb = cpp.get("/api/v1/render/fonts").json()
        compare("render/fonts", py.get("/api/v1/render/fonts"), cpp.get("/api/v1/render/fonts"))
        if fa.get("default") != fb.get("default"):
            note(f"默认字体不同（可接受但需记录）: py={fa.get('default')} cpp={fb.get('default')}")
        ids_a = [f["id"] for f in fa.get("fonts", [])]
        ids_b = [f["id"] for f in fb.get("fonts", [])]
        if ids_a != ids_b:
            note(f"字体集不同: 仅 py 有 {sorted(set(ids_a) - set(ids_b))[:5]}, "
                 f"仅 cpp 有 {sorted(set(ids_b) - set(ids_a))[:5]}")
        for fid in ids_a[:2]:
            get_pair(py, cpp, f"font file {fid}", f"/api/v1/render/fonts/{fid}/file")
        get_pair(py, cpp, "font file 不存在", "/api/v1/render/fonts/nope/file")
        get_pair(py, cpp, "font file 目录穿越", "/api/v1/render/fonts/..%2F..%2Fconfig.yaml/file")

        # ---------------------------------------------------------- /files
        get_pair(py, cpp, "files /", "/api/v1/files")
        get_pair(py, cpp, "files 不存在", "/api/v1/files", {"path": "/nope"})
        get_pair(py, cpp, "files 穿越", "/api/v1/files", {"path": "/../.."})
        get_pair(py, cpp, "files 穿越2", "/api/v1/files", {"path": "/lib/../../etc"})
        get_pair(py, cpp, "files limit=0", "/api/v1/files", {"limit": 0})
        get_pair(py, cpp, "files limit=99999", "/api/v1/files", {"limit": 99999})
        get_pair(py, cpp, "files limit=abc", "/api/v1/files", {"limit": "abc"})
        get_pair(py, cpp, "files offset=-1", "/api/v1/files", {"offset": -1})

        # ---------------------------------------------------------- 未知路由/方法
        get_pair(py, cpp, "未知路由", "/api/v1/nope")
        a = py.post("/api/v1/files"); b = cpp.post("/api/v1/files")
        compare("POST /files（方法不允许）", a, b)

        # ---------------------------------------------------------- 书籍
        books = walk(py)
        books_cpp = walk(cpp)
        if [e["path"] for e in books] != [e["path"] for e in books_cpp]:
            record("文件树", f"py={[e['path'] for e in books]} cpp={[e['path'] for e in books_cpp]}")
        ids_map = {e["path"]: e["book_id"] for e in books}
        ids_map_cpp = {e["path"]: e["book_id"] for e in books_cpp}
        for p, bid in ids_map.items():
            if ids_map_cpp.get(p) != bid:
                record(f"book_id[{p}]", f"{bid} (py) vs {ids_map_cpp.get(p)} (cpp)")

        for entry in books:
            bid = entry["book_id"]
            label = entry["path"]
            get_pair(py, cpp, f"meta {label}", f"/api/v1/books/{bid}/meta")
            get_pair(py, cpp, f"meta 不存在 {label}", "/api/v1/books/deadbeefdeadbeef/meta")
            get_pair(py, cpp, f"cover {label}", f"/api/v1/books/{bid}/cover", {"w": 240})
            get_pair(py, cpp, f"cover w=40 {label}", f"/api/v1/books/{bid}/cover", {"w": 40})
            get_pair(py, cpp, f"cover w=39 {label}", f"/api/v1/books/{bid}/cover", {"w": 39})
            get_pair(py, cpp, f"cover w=1025 {label}", f"/api/v1/books/{bid}/cover", {"w": 1025})
            get_pair(py, cpp, f"cover w=abc {label}", f"/api/v1/books/{bid}/cover", {"w": "abc"})

            # 封面 ETag / 304
            r = py.get(f"/api/v1/books/{bid}/cover", params={"w": 240})
            et = r.headers.get("etag")
            if et:
                get_pair(py, cpp, f"cover 304 {label}", f"/api/v1/books/{bid}/cover",
                         {"w": 240}, {"If-None-Match": et})

            # ---- 排版 + 页 ----
            mode = "flow"
            for pname in ("flow", "flow_big", "fixed", "strip"):
                prof = PROFILES[pname]
                a = py.post(f"/api/v1/books/{bid}/layout", json=prof)
                b = cpp.post(f"/api/v1/books/{bid}/layout", json=prof)
                compare(f"layout[{pname}] {label}", a, b)
                if a.status_code != 200 or b.status_code != 200:
                    continue
                ja, jb = a.json(), b.json()
                if pname == "flow":
                    mode = ja.get("mode", "flow")
                if ja.get("mode") != jb.get("mode"):
                    continue
                lid = ja["layout_id"]
                total = ja["total_pages"]
                effective = ja["mode"]

                pages = sorted({1, 2, max(1, total // 2), max(1, total)})
                for pg in pages:
                    get_pair(py, cpp, f"pages/{pg} {pname} {label}",
                             f"/api/v1/books/{bid}/pages/{pg}", {"layout_id": lid})
                    get_pair(py, cpp, f"pages/{pg}/meta {pname} {label}",
                             f"/api/v1/books/{bid}/pages/{pg}/meta", {"layout_id": lid})
                get_pair(py, cpp, f"pages/0 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/0", {"layout_id": lid})
                get_pair(py, cpp, f"pages/{total + 1} {pname} {label}",
                         f"/api/v1/books/{bid}/pages/{total + 1}", {"layout_id": lid})
                get_pair(py, cpp, f"pages/abc {pname} {label}",
                         f"/api/v1/books/{bid}/pages/abc", {"layout_id": lid})
                get_pair(py, cpp, f"pages 无 layout_id {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1")
                get_pair(py, cpp, f"pages 坏 layout_id {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": "L000000000000"})

                # format / scale / quality / pair / rtl 边界
                get_pair(py, cpp, f"format=bogus {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "format": "bogus"})
                get_pair(py, cpp, f"fit=bogus {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "fit": "bogus"})
                get_pair(py, cpp, f"format=json {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "format": "json"})
                get_pair(py, cpp, f"scale=0.1 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "scale": 0.1})
                get_pair(py, cpp, f"scale=9 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "scale": 9})
                get_pair(py, cpp, f"scale=abc {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "scale": "abc"})
                get_pair(py, cpp, f"quality=0 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "quality": 0})
                get_pair(py, cpp, f"quality=101 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "quality": 101})
                get_pair(py, cpp, f"pair=2 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "pair": 2})
                get_pair(py, cpp, f"rtl=2 {pname} {label}",
                         f"/api/v1/books/{bid}/pages/1", {"layout_id": lid, "rtl": 2})

                # 页 ETag / 304
                r = py.get(f"/api/v1/books/{bid}/pages/1", params={"layout_id": lid})
                et = r.headers.get("etag")
                if et:
                    get_pair(py, cpp, f"pages 304 {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1", {"layout_id": lid},
                             {"If-None-Match": et})
                    get_pair(py, cpp, f"pages 304 * {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1", {"layout_id": lid},
                             {"If-None-Match": "*"})

                if effective in ("fixed", "strip"):
                    # Range 语义
                    for rng in ("bytes=0-99", "bytes=-50", "bytes=999999999-",
                                "bytes=abc", "bytes=-", "bytes=100-50", "bytes=0-99,200-299"):
                        get_pair(py, cpp, f"range {rng} {pname} {label}",
                                 f"/api/v1/books/{bid}/pages/1",
                                 {"layout_id": lid, "fit": "original"},
                                 {"Range": rng})
                    get_pair(py, cpp, f"fit=original {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1",
                             {"layout_id": lid, "fit": "original"})
                    get_pair(py, cpp, f"pair=1 {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1",
                             {"layout_id": lid, "pair": 1})
                    get_pair(py, cpp, f"pair=1 rtl=1 {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1",
                             {"layout_id": lid, "pair": 1, "rtl": 1})
                    get_pair(py, cpp, f"quality=50 {pname} {label}",
                             f"/api/v1/books/{bid}/pages/1",
                             {"layout_id": lid, "quality": 50})

                # ---- locate ----
                for (ch, blk, c) in ((0, 0, 0), (1, 0, 0), (2, 0, 5), (99, 0, 0), (-1, 0, 0)):
                    get_pair(py, cpp, f"locate {pname} {label} ({ch},{blk},{c})",
                             f"/api/v1/books/{bid}/locate",
                             {"layout_id": lid, "chapter": ch, "block": blk, "char": c})

                # ---- search ----
                if effective == "flow":
                    for q in ("第", "a", "不存在的词xyz", ""):
                        get_pair(py, cpp, f"search {label} q={q!r}",
                                 f"/api/v1/books/{bid}/search",
                                 {"q": q, "layout_id": lid})
                    get_pair(py, cpp, f"search 无 layout {label}",
                             f"/api/v1/books/{bid}/search", {"q": "第"})
                    get_pair(py, cpp, f"search limit=0 {label}",
                             f"/api/v1/books/{bid}/search",
                             {"q": "第", "layout_id": lid, "limit": 0})
                    get_pair(py, cpp, f"search limit=999 {label}",
                             f"/api/v1/books/{bid}/search",
                             {"q": "第", "layout_id": lid, "limit": 999})
                else:
                    get_pair(py, cpp, f"search fixed {label}",
                             f"/api/v1/books/{bid}/search", {"q": "第"})

                # ---- asset ----
                get_pair(py, cpp, f"asset 无 path {label}", f"/api/v1/books/{bid}/asset")
                get_pair(py, cpp, f"asset 穿越 {label}", f"/api/v1/books/{bid}/asset",
                         {"path": "../../config.yaml"})
                get_pair(py, cpp, f"asset 不存在 {label}", f"/api/v1/books/{bid}/asset",
                         {"path": "nope/nope.png"})

            # ---- layout 请求体校验 ----
            bad_bodies = {
                "空对象": {},
                "viewport 缺失": {"mode": "flow"},
                "viewport 越界": {**PROFILES["flow"], "viewport": {"w": 0, "h": 1920}},
                "font 越界": {**PROFILES["flow"], "font": {"family": "auto", "size_px": 4, "line_height": 1.6}},
                "line_height 越界": {**PROFILES["flow"], "font": {"family": "auto", "size_px": 52, "line_height": 99}},
                "margin 负": {**PROFILES["flow"], "margin": {"left": -1, "right": 0, "top": 0, "bottom": 0}},
                "mode 非法": {**PROFILES["flow"], "mode": "bogus"},
                "未知键": {**PROFILES["flow"], "unknown_key": 1},
                "类型错": {**PROFILES["flow"], "viewport": {"w": "1080", "h": 1920}},
                "字符串 body": "not json",
            }
            for bname, body in bad_bodies.items():
                if isinstance(body, str):
                    a = py.post(f"/api/v1/books/{bid}/layout",
                                content=body, headers={"Content-Type": "application/json"})
                    b = cpp.post(f"/api/v1/books/{bid}/layout",
                                 content=body, headers={"Content-Type": "application/json"})
                else:
                    a = py.post(f"/api/v1/books/{bid}/layout", json=body)
                    b = cpp.post(f"/api/v1/books/{bid}/layout", json=body)
                compare(f"layout 校验[{bname}] {label}", a, b)

        # ---------------------------------------------------------- 进度
        bid = books[0]["book_id"] if books else "deadbeefdeadbeef"
        get_pair(py, cpp, "progress 无记录", f"/api/v1/progress/{bid}")
        get_pair(py, cpp, "progress 坏书", "/api/v1/progress/deadbeefdeadbeef")
        put_ok = put_bad = None
        payload = {"anchor": {"chapter": 1, "block": 2, "char": 3}, "page": 7, "ratio": 0.5}
        a = py.put(f"/api/v1/progress/{bid}", params={"device_id": "dev1"}, json=payload)
        b = cpp.put(f"/api/v1/progress/{bid}", params={"device_id": "dev1"}, json=payload)
        compare("progress PUT", a, b)
        get_pair(py, cpp, "progress GET 指定 device", f"/api/v1/progress/{bid}",
                 {"device_id": "dev1"})
        get_pair(py, cpp, "progress GET 任意 device", f"/api/v1/progress/{bid}")
        get_pair(py, cpp, "progress GET 未知 device", f"/api/v1/progress/{bid}",
                 {"device_id": "nobody"})
        for bname, body in {
            "ratio 越界": {"anchor": {"chapter": 0, "block": 0, "char": 0}, "page": 1, "ratio": 5},
            "ratio 负": {"anchor": {"chapter": 0, "block": 0, "char": 0}, "ratio": -3},
            "空 body": {},
            "page 负数": {"page": -1},
            "anchor 缺字段": {"anchor": {}},
            "坏 json": "{",
        }.items():
            if isinstance(body, str):
                a = py.put(f"/api/v1/progress/{bid}", content=body,
                           headers={"Content-Type": "application/json"})
                b = cpp.put(f"/api/v1/progress/{bid}", content=body,
                            headers={"Content-Type": "application/json"})
            else:
                a = py.put(f"/api/v1/progress/{bid}", json=body)
                b = cpp.put(f"/api/v1/progress/{bid}", json=body)
            compare(f"progress PUT 校验[{bname}]", a, b)

        # ---------------------------------------------------------- 图集
        post_pair(py, cpp, "open-folder 图集", "/api/v1/books/open-folder", {"path": "/lib/图集"})
        post_pair(py, cpp, "open-folder 单图", "/api/v1/books/open-folder",
                  {"path": "/lib/图集/img_01.png"})
        post_pair(py, cpp, "open-folder 不存在", "/api/v1/books/open-folder", {"path": "/lib/nope"})
        post_pair(py, cpp, "open-folder 无 path", "/api/v1/books/open-folder", {})
        post_pair(py, cpp, "open-folder 穿越", "/api/v1/books/open-folder", {"path": "/../.."})
        post_pair(py, cpp, "open-folder 普通文件", "/api/v1/books/open-folder",
                  {"path": "/lib/小说/长篇小说.txt"})

        # ---------------------------------------------------------- 书内资源（EPUB 真实路径）
        epub = next((e for e in books if e["path"].endswith(".epub")), None)
        if epub:
            try:
                with zipfile.ZipFile(LIB / "EPUB" / "示例电子书.epub") as zf:
                    asset = next(n for n in zf.namelist()
                                 if re.search(r"\.(png|jpe?g|gif)$", n, re.I))
                get_pair(py, cpp, "asset 真实资源", f"/api/v1/books/{epub['book_id']}/asset",
                         {"path": asset})
            except Exception as exc:  # noqa: BLE001
                note(f"未能取到 EPUB 内资源路径：{exc}")

    # ---------------------------------------------------------- 报告
    print(f"比对用例: {checks}")
    if notes:
        print("\n== 说明（非差异） ==")
        for n in notes:
            print(f"  - {n}")
    if diffs:
        print(f"\n== 差异 {len(diffs)} 项 ==")
        for d in diffs:
            print(f"  * {d}")
        return 1
    print("\n无差异")
    return 0


if __name__ == "__main__":
    sys.exit(main())
