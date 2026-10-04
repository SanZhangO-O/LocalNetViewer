"""定位 C++ 与 Python 在 flow 分页上的差异来源（review 用）。

对比两边的 format=json 每页块结构：先比"每块行数"（折行差异），再比"每页块分布"
（分页差异）。用法：python -X utf8 locate_paging_diff.py <book_path> <profile>
"""

from __future__ import annotations

import json
import sys
from collections import Counter

import httpx

PY = "http://127.0.0.1:8901"
CPP = "http://127.0.0.1:8902"

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
}


def book_id(client: httpx.Client, path: str) -> str:
    folder = path.rsplit("/", 1)[0]
    name = path.rsplit("/", 1)[1]
    data = client.get("/api/v1/files", params={"path": folder}).json()
    return next(e["book_id"] for e in data["entries"] if e["name"] == name)


def pages_json(client: httpx.Client, bid: str, lid: str, total: int) -> list:
    out = []
    for p in range(1, total + 1):
        r = client.get(f"/api/v1/books/{bid}/pages/{p}",
                       params={"layout_id": lid, "format": "json"})
        out.append(r.json() if r.status_code == 200 else {"error": r.status_code})
    return out


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/lib/EPUB/示例电子书.epub"
    pname = sys.argv[2] if len(sys.argv) > 2 else "flow_big"
    prof = PROFILES[pname]
    with httpx.Client(timeout=120.0, base_url=PY) as py, \
            httpx.Client(timeout=120.0, base_url=CPP) as cpp:
        bid_py, bid_cpp = book_id(py, path), book_id(cpp, path)
        lay_py = py.post(f"/api/v1/books/{bid_py}/layout", json=prof).json()
        lay_cpp = cpp.post(f"/api/v1/books/{bid_cpp}/layout", json=prof).json()
        print(f"layout_id: py={lay_py['layout_id']} cpp={lay_cpp['layout_id']}")
        print(f"total_pages: py={lay_py['total_pages']} cpp={lay_cpp['total_pages']}")
        a = pages_json(py, bid_py, lay_py["layout_id"], lay_py["total_pages"])
        b = pages_json(cpp, bid_cpp, lay_cpp["layout_id"], lay_cpp["total_pages"])

        # 1) 每块行数（折行是否一致）
        def line_counts(pages):
            c = Counter()
            for pg in pages:
                for blk in pg.get("blocks", []):
                    key = (blk.get("type"), blk.get("level"), blk.get("font_size"),
                           tuple(blk.get("lines", []))[:1])
                    c[key] += len(blk.get("lines", []))
            return c

        la, lb = line_counts(a), line_counts(b)
        only_py = {k: v for k, v in la.items() if lb.get(k) != v}
        print(f"\n折行不同的块数: {len(only_py)}")
        for k, v in list(only_py.items())[:5]:
            print(f"  py={v} cpp={lb.get(k)} key={k}")

        # 2) 每页高度（分页是否一致）
        for i in range(min(len(a), len(b))):
            ha = sum(blk.get("height", 0) for blk in a[i].get("blocks", []))
            hb = sum(blk.get("height", 0) for blk in b[i].get("blocks", []))
            if abs(ha - hb) > 0.01 or len(a[i].get("blocks", [])) != len(b[i].get("blocks", [])):
                print(f"\n首个分页差异 page={i+1}: py 高度={ha} 块数={len(a[i].get('blocks', []))} "
                      f"cpp 高度={hb} 块数={len(b[i].get('blocks', []))}")
                print(f"  py  anchor={a[i].get('anchor')} lines={[len(x.get('lines', [])) for x in a[i]['blocks']]}")
                print(f"  cpp anchor={b[i].get('anchor')} lines={[len(x.get('lines', [])) for x in b[i]['blocks']]}")
                break
        else:
            print("\n所有页的分页结构一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
