"""最小复现：只做 /files 树遍历 + 每本书的 meta/cover，观察 C++ 是否崩溃。"""

from __future__ import annotations

import sys

import httpx

CPP = "http://127.0.0.1:8902"


def walk(client, path="/", depth=0):
    if depth > 4:
        return []
    out = []
    data = client.get("/api/v1/files", params={"path": path}).json()
    for e in data.get("entries", []):
        if e.get("type") == "dir":
            out.extend(walk(client, e["path"], depth + 1))
        elif e.get("book_id"):
            out.append(e)
    return out


def main() -> int:
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    with httpx.Client(timeout=60.0, base_url=CPP) as c:
        if phase in ("all", "files"):
            print("phase files")
            for path in ("/", "/lib", "/lib/小说", "/lib/EPUB", "/lib/FB2", "/lib/PDF",
                         "/lib/漫画", "/lib/图集"):
                r = c.get("/api/v1/files", params={"path": path})
                print("  ", path, r.status_code)
            books = walk(c)
            print("   books:", len(books))
        else:
            books = walk(c)
        if phase in ("all", "meta"):
            print("phase meta")
            for e in books:
                bid = e["book_id"]
                for p in ({"w": 240}, {"w": 40}, {"w": 39}, {"w": 1025}, {"w": "abc"}):
                    r = c.get(f"/api/v1/books/{bid}/cover", params=p)
                    print("   cover", e["path"], p, r.status_code)
                r = c.get(f"/api/v1/books/{bid}/meta")
                print("   meta", e["path"], r.status_code)
        if phase in ("all", "pages"):
            print("phase pages")
            for e in books:
                bid = e["book_id"]
                prof = {"viewport": {"w": 1080, "h": 1920},
                        "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
                        "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
                        "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
                        "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
                        "chapter_new_page": True, "mode": "flow"}
                lay = c.post(f"/api/v1/books/{bid}/layout", json=prof)
                if lay.status_code != 200:
                    print("   layout", e["path"], lay.status_code)
                    continue
                lid = lay.json()["layout_id"]
                for p in range(1, lay.json()["total_pages"] + 1):
                    r = c.get(f"/api/v1/books/{bid}/pages/{p}", params={"layout_id": lid})
                    if r.status_code != 200:
                        print("   page", e["path"], p, r.status_code)
                print("   pages ok", e["path"], lay.json()["total_pages"])
    print("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
