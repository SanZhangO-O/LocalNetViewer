"""探测 Python 版 pydantic 校验的确切行为（review 用，不属于发布内容）。

对同一组非法/边界输入打印 Python 服务器的真实响应，供 C++ 侧逐字对齐。
用法：.venv\\Scripts\\python.exe -X utf8 ..\\.e2e\\review\\probe_pydantic.py
"""

from __future__ import annotations

import json
import sys

import httpx

BASE = "http://127.0.0.1:8901"

PROFILE = {
    "viewport": {"w": 1080, "h": 1920},
    "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "flow",
}


def show(label: str, resp: httpx.Response) -> None:
    body = resp.text
    try:
        body = json.dumps(resp.json(), ensure_ascii=False)
    except Exception:  # noqa: BLE001
        pass
    print(f"{label}\n    {resp.status_code} {body[:600]}")


def main() -> int:
    with httpx.Client(timeout=60.0, base_url=BASE) as c:
        files = c.get("/api/v1/files", params={"path": "/lib/小说"}).json()
        bid = next(e["book_id"] for e in files["entries"] if e.get("book_id"))
        print(f"# book_id={bid}")

        # ---------- layout body 校验 ----------
        cases = {
            "w=字符串数字": {**PROFILE, "viewport": {"w": "1080", "h": 1920}},
            "w=字符串非数字": {**PROFILE, "viewport": {"w": "abc", "h": 1920}},
            "w=小数": {**PROFILE, "viewport": {"w": 10.5, "h": 1920}},
            "w=小数整": {**PROFILE, "viewport": {"w": 10.0, "h": 1920}},
            "w=bool": {**PROFILE, "viewport": {"w": True, "h": 1920}},
            "w=null": {**PROFILE, "viewport": {"w": None, "h": 1920}},
            "w 缺失": {**PROFILE, "viewport": {"h": 1920}},
            "w=0 且 h=0": {**PROFILE, "viewport": {"w": 0, "h": 0}},
            "viewport=数组": {**PROFILE, "viewport": [1080, 1920]},
            "font 多错": {**PROFILE, "font": {"size_px": 4, "line_height": 9}},
            "size_px 字符串": {**PROFILE, "font": {"size_px": "52"}},
            "line_height 字符串数字": {**PROFILE, "font": {"line_height": "1.6"}},
            "letter_spacing 越界": {**PROFILE, "font": {"letter_spacing_px": 99}},
            "max_width_ratio=0": {**PROFILE, "image": {"max_width_ratio": 0}},
            "spacing 越界": {**PROFILE, "paragraph": {"spacing_px": 999}},
            "indent 越界": {**PROFILE, "paragraph": {"first_line_indent_chars": 99}},
            "chapter_new_page=字符串": {**PROFILE, "chapter_new_page": "true"},
            "chapter_new_page=1": {**PROFILE, "chapter_new_page": 1},
            "strip_overlap 越界": {**PROFILE, "strip_overlap_px": 9999},
            "mode=大写": {**PROFILE, "mode": "FLOW"},
            "未知键": {**PROFILE, "nope": 1},
            "margin 缺省": {k: v for k, v in PROFILE.items() if k != "margin"},
            "body=数组": [1, 2],
            "body=null": None,
        }
        for name, body in cases.items():
            show(f"# layout[{name}]", c.post(f"/api/v1/books/{bid}/layout", json=body))

        # ---------- query 校验 ----------
        qcases = [
            ("files limit=0", "/api/v1/files", {"limit": 0}),
            ("files limit=10.5", "/api/v1/files", {"limit": "10.5"}),
            ("files limit=10.0", "/api/v1/files", {"limit": "10.0"}),
            ("files limit= 10", "/api/v1/files", {"limit": " 10"}),
            ("files limit=+10", "/api/v1/files", {"limit": "+10"}),
            ("files limit=", "/api/v1/files", {"limit": ""}),
            ("files limit=true", "/api/v1/files", {"limit": "true"}),
            ("files offset=0", "/api/v1/files", {"offset": 0}),
            ("cover w=39.5", f"/api/v1/books/{bid}/cover", {"w": "39.5"}),
            ("cover w=40.0", f"/api/v1/books/{bid}/cover", {"w": "40.0"}),
            ("pages scale=0.5", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "scale": "0.5"}),
            ("pages scale=nan", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "scale": "nan"}),
            ("pages scale=inf", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "scale": "inf"}),
            ("pages scale=1e0", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "scale": "1e0"}),
            ("pages quality=abc", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "quality": "abc"}),
            ("pages pair=true", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "pair": "true"}),
            ("pages pair=1", f"/api/v1/books/{bid}/pages/1", {"layout_id": "x", "pair": "1"}),
            ("pages page=abc", f"/api/v1/books/{bid}/pages/abc", {"layout_id": "x"}),
            ("pages page=-1", f"/api/v1/books/{bid}/pages/-1", {"layout_id": "x"}),
            ("pages page=1.5", f"/api/v1/books/{bid}/pages/1.5", {"layout_id": "x"}),
            ("locate chapter=-1", f"/api/v1/books/{bid}/locate", {"layout_id": "x", "chapter": "-1"}),
            ("locate 全缺", f"/api/v1/books/{bid}/locate", {}),
            ("search q 缺", f"/api/v1/books/{bid}/search", {}),
            ("search q 空", f"/api/v1/books/{bid}/search", {"q": ""}),
            ("search limit=-1", f"/api/v1/books/{bid}/search", {"q": "a", "limit": "-1"}),
        ]
        for name, path, params in qcases:
            show(f"# {name}", c.get(path, params=params))

        # ---------- progress body ----------
        pcases = {
            "anchor 缺": {"page": 1},
            "anchor=null": {"anchor": None},
            "anchor 空对象": {"anchor": {}},
            "anchor 字段字符串": {"anchor": {"chapter": "2"}},
            "ratio=5": {"anchor": {}, "ratio": 5},
            "ratio=-3": {"anchor": {}, "ratio": -3},
            "page=1.5": {"anchor": {}, "page": 1.5},
            "page=null": {"anchor": {}, "page": None},
            "ratio=字符串": {"anchor": {}, "ratio": "0.5"},
        }
        for name, body in pcases.items():
            show(f"# progress[{name}]", c.put(f"/api/v1/progress/{bid}", json=body))

        # ---------- 路径/方法 ----------
        show("# 未知路由", c.get("/api/v1/nope"))
        show("# 未知路由深", c.get("/api/v1/books/x/y/z"))
        show("# 方法不允许", c.post("/api/v1/files"))
        show("# 方法不允许2", c.delete(f"/api/v1/books/{bid}/meta"))
        show("# HEAD /healthz", c.head("/healthz"))
        show("# OPTIONS /api/v1/files", c.options("/api/v1/files"))
        show("# 尾斜杠", c.get("/api/v1/files/"))
        show("# 字体不存在", c.get("/api/v1/render/fonts/nope/file"))
        show("# 字体空 id", c.get("/api/v1/render/fonts//file"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
