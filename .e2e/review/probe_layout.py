"""补充探测：pydantic 子模型/空值的确切错误文案（review 用）。"""

from __future__ import annotations

import json
import sys

import httpx

BASE = "http://127.0.0.1:8901"
P = {
    "viewport": {"w": 1080, "h": 1920},
    "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "flow",
}


def main() -> int:
    with httpx.Client(timeout=60.0, base_url=BASE) as c:
        files = c.get("/api/v1/files", params={"path": "/lib/小说"}).json()
        bid = next(e["book_id"] for e in files["entries"] if e.get("book_id"))
        cases = {
            "margin=null": {**P, "margin": None},
            "font=null": {**P, "font": None},
            "paragraph=null": {**P, "paragraph": None},
            "image=null": {**P, "image": None},
            "viewport=null": {**P, "viewport": None},
            "viewport 缺 w": {**P, "viewport": {"h": 1920}},
            "mode=null": {**P, "mode": None},
            "strip_overlap=null": {**P, "strip_overlap_px": None},
            "chapter_new_page=null": {**P, "chapter_new_page": None},
            "font.size_px=null": {**P, "font": {"size_px": None}},
            "font.family=null": {**P, "font": {"family": None}},
            "font.family=数字": {**P, "font": {"family": 5}},
            "paragraph.spacing=null": {**P, "paragraph": {"spacing_px": None}},
            "indent=null": {**P, "paragraph": {"first_line_indent_chars": None}},
            "image.ratio=null": {**P, "image": {"max_width_ratio": None}},
            "margin.left=null": {**P, "margin": {"left": None}},
            "margin=5": {**P, "margin": 5},
            "mode=5": {**P, "mode": 5},
            "strip_overlap=字符串": {**P, "strip_overlap_px": "40"},
            "viewport 多错": {**P, "viewport": {"w": 0, "h": "x"}},
        }
        for name, body in cases.items():
            r = c.post(f"/api/v1/books/{bid}/layout", json=body)
            print(f"{name}: {r.status_code} {json.dumps(r.json(), ensure_ascii=False)[:400]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
