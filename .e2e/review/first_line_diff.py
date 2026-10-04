"""找出两边第一处折行差异，并用 Pillow 量出该行的宽度与行宽上限（review 用）。"""

from __future__ import annotations

import sys

import httpx
from PIL import ImageFont

PY = "http://127.0.0.1:8901"
CPP = "http://127.0.0.1:8902"
PROF = {
    "viewport": {"w": 1080, "h": 1920},
    "margin": {"left": 60, "right": 60, "top": 60, "bottom": 120},
    "font": {"family": "auto", "size_px": 88, "line_height": 2.0},
    "paragraph": {"spacing_px": 30, "first_line_indent_chars": 0},
    "image": {"max_width_ratio": 0.8, "max_height_ratio": 0.5},
    "chapter_new_page": False,
    "mode": "flow",
}


def lines_of(client, bid, lid, total):
    out = []
    for p in range(1, total + 1):
        r = client.get(f"/api/v1/books/{bid}/pages/{p}",
                       params={"layout_id": lid, "format": "json"})
        if r.status_code != 200:
            continue
        for blk in r.json()["blocks"]:
            for ln in blk.get("lines", []):
                out.append((p, blk.get("type"), ln))
    return out


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/lib/EPUB/示例电子书.epub"
    folder, name = path.rsplit("/", 1)
    with httpx.Client(timeout=120.0, base_url=PY) as py, \
            httpx.Client(timeout=120.0, base_url=CPP) as cpp:
        bid_py = next(e["book_id"] for e in
                      py.get("/api/v1/files", params={"path": folder}).json()["entries"]
                      if e["name"] == name)
        bid_cpp = next(e["book_id"] for e in
                       cpp.get("/api/v1/files", params={"path": folder}).json()["entries"]
                       if e["name"] == name)
        la = py.post(f"/api/v1/books/{bid_py}/layout", json=PROF).json()
        lb = cpp.post(f"/api/v1/books/{bid_cpp}/layout", json=PROF).json()
        a = lines_of(py, bid_py, la["layout_id"], la["total_pages"])
        b = lines_of(cpp, bid_cpp, lb["layout_id"], lb["total_pages"])
        print(f"py 行数={len(a)} cpp 行数={len(b)}  py 页={la['total_pages']} cpp 页={lb['total_pages']}")
        font = ImageFont.truetype(r"C:\Windows\Fonts\Deng.ttf", 88)
        limit = 1080 - 60 - 60
        print(f"行宽上限={limit}")
        shown = 0
        for i in range(min(len(a), len(b))):
            if a[i][2] != b[i][2]:
                pa, ta, sa = a[i]
                pb, tb, sb = b[i]
                print(f"\n第 {i} 行起不同（py page {pa} / cpp page {pb}）")
                print(f"  py : {sa!r}  width={font.getlength(sa):.3f}")
                print(f"  cpp: {sb!r}  width={font.getlength(sb):.3f}")
                if i + 1 < len(a):
                    print(f"  py 下一行 : {a[i+1][2]!r}")
                if i + 1 < len(b):
                    print(f"  cpp 下一行: {b[i+1][2]!r}")
                shown += 1
                if shown >= 3:
                    break
        if shown == 0:
            print("前 %d 行完全一致" % min(len(a), len(b)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
