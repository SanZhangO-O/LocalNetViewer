"""EPUB / PDF / FB2 三种格式的端到端验证（此前从未真正渲染过）。

用法（服务器需已在 8765 运行，且已执行 setup_formats.py）：
    server\\.venv\\Scripts\\python.exe -X utf8 .e2e\\smoke_formats.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import httpx

BASE = "http://127.0.0.1:8765/api/v1"
OUT = Path(__file__).resolve().parent / "shots"
OUT.mkdir(parents=True, exist_ok=True)

PROFILE = {
    "viewport": {"w": 1080, "h": 1920},
    "margin": {"left": 40, "right": 40, "top": 104, "bottom": 180},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "flow",
}

failures: list[str] = []


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"[{'OK  ' if ok else 'FAIL'}] {label} {detail}", flush=True)
    if not ok:
        failures.append(label)


def find_book(client: httpx.Client, folder: str, name: str) -> str:
    data = client.get(f"{BASE}/files", params={"path": folder}).json()
    for entry in data["entries"]:
        if entry["name"] == name:
            return entry["book_id"]
    raise AssertionError(f"{name} 不在 {folder}: {[e['name'] for e in data['entries']]}")


def main() -> int:
    with httpx.Client(timeout=120.0) as client:
        # ------------------------------------------------------------ EPUB
        print("== EPUB ==", flush=True)
        bid = find_book(client, "/测试库/EPUB", "示例电子书.epub")
        meta = client.get(f"{BASE}/books/{bid}/meta").json()
        check("EPUB 元数据", meta["mode"] == "flow", f"format={meta['format']}")
        check("EPUB 标题", meta["title"] == "示例电子书", meta["title"])
        check("EPUB 作者", meta["author"] == "端到端验证", str(meta["author"]))
        check("EPUB 语言", meta["language"] in ("zh-CN", "zh"), str(meta["language"]))
        check("EPUB 章节数 = 4", len(meta["chapters"]) == 4, f"{len(meta['chapters'])}")
        titles = [c["title"] for c in meta["chapters"]]
        check("EPUB 目录来自 nav", titles[0] == "第1章 EPUB 测试", str(titles))

        layout = client.post(f"{BASE}/books/{bid}/layout", json=PROFILE).json()
        total = layout["total_pages"]
        check("EPUB 排版", total > 4, f"layout={layout['layout_id']} pages={total}")
        starts = [c["start_page"] for c in layout["chapters"]]
        check("EPUB 章节起始页递增", starts == sorted(s for s in starts if s), str(starts))

        page1 = client.get(
            f"{BASE}/books/{bid}/pages/1", params={"layout_id": layout["layout_id"], "theme": "white"}
        )
        (OUT / "epub_page_1.png").write_bytes(page1.content)
        check("EPUB 渲染第 1 页", page1.status_code == 200 and len(page1.content) > 3000,
              f"{len(page1.content)} bytes")

        # 内嵌图片：EPUB 的图片块应通过 /asset 提供
        img_page = None
        for p in range(1, total + 1):
            meta_p = client.get(
                f"{BASE}/books/{bid}/pages/{p}/meta", params={"layout_id": layout["layout_id"]}
            ).json()
            if meta_p["chapter_index"] >= 0:
                img_page = p
                break
        js = client.get(
            f"{BASE}/books/{bid}/pages/{img_page or 1}",
            params={"layout_id": layout["layout_id"], "format": "json"},
        ).json()
        check("EPUB JSON 模式可用", "blocks" in js)
        check("JSON 不重复缩进", js["paragraph"]["first_line_indent_chars"] == 0)

        assets = [b for b in js["blocks"] if b["type"] == "image"]
        # 图在章节末尾，逐页找一次内嵌资源
        asset_ok = False
        asset_detail = "未找到 image 块"
        for p in range(1, total + 1):
            js_p = client.get(
                f"{BASE}/books/{bid}/pages/{p}",
                params={"layout_id": layout["layout_id"], "format": "json"},
            ).json()
            for b in js_p["blocks"]:
                if b["type"] == "image":
                    resp = client.get("http://127.0.0.1:8765" + b["asset"])
                    asset_ok = resp.status_code == 200 and resp.headers["content-type"].startswith("image/")
                    asset_detail = f"{b['asset'][-40:]} -> {resp.status_code} {len(resp.content)}B"
                    break
            if asset_ok:
                break
        check("EPUB 内嵌图片可取", asset_ok, asset_detail)

        cover = client.get(f"{BASE}/books/{bid}/cover", params={"w": 300})
        (OUT / "epub_cover.png").write_bytes(cover.content)
        check("EPUB 封面", cover.status_code == 200 and cover.headers["content-type"].startswith("image/"),
              f"{len(cover.content)} bytes")

        # 锚点往返
        rt = True
        for p in (1, total // 2, total):
            m = client.get(f"{BASE}/books/{bid}/pages/{p}/meta",
                           params={"layout_id": layout["layout_id"]}).json()
            loc = client.get(f"{BASE}/books/{bid}/locate", params={
                "layout_id": layout["layout_id"],
                "chapter": m["anchor"]["chapter"], "block": m["anchor"]["block"],
                "char": m["anchor"]["char"]}).json()
            if loc["page"] != p or loc["clamped"] or loc["chapter_index"] != m["chapter_index"]:
                rt = False
        check("EPUB 锚点往返 + 章节一致", rt)

        search = client.get(f"{BASE}/books/{bid}/search",
                            params={"q": "第3章", "layout_id": layout["layout_id"]}).json()
        check("EPUB 全文搜索", search["total"] >= 1 and search["results"][0]["page"],
              f"命中 {search['total']}")

        # ------------------------------------------------------------ PDF
        print("== PDF ==", flush=True)
        pid = find_book(client, "/测试库/PDF", "示例文档.pdf")
        pmeta = client.get(f"{BASE}/books/{pid}/meta").json()
        check("PDF 元数据", pmeta["mode"] == "fixed" and pmeta["page_count"] == 5,
              f"pages={pmeta['page_count']}")
        pl = client.post(f"{BASE}/books/{pid}/layout",
                         json={**PROFILE, "mode": "fixed"}).json()
        check("PDF 排版", pl["total_pages"] == 5, f"{pl['total_pages']}")
        ppage = client.get(f"{BASE}/books/{pid}/pages/1",
                           params={"layout_id": pl["layout_id"], "fit": "viewport"})
        (OUT / "pdf_page_1.jpg").write_bytes(ppage.content)
        check("PDF 渲染第 1 页", ppage.status_code == 200 and len(ppage.content) > 5000,
              f"{len(ppage.content)} bytes")
        pmeta_page = client.get(f"{BASE}/books/{pid}/pages/1/meta",
                                params={"layout_id": pl["layout_id"]}).json()
        check("PDF 页高宽比 ≈ A4", abs((pmeta_page["aspect"] or 0) - 1754 / 1240) < 0.05,
              f"aspect={pmeta_page['aspect']:.3f}")
        porig = client.get(f"{BASE}/books/{pid}/pages/1",
                           params={"layout_id": pl["layout_id"], "fit": "original"})
        check("PDF 原图模式", porig.status_code == 200 and len(porig.content) > 10000,
              f"{len(porig.content)} bytes")
        ppair = client.get(f"{BASE}/books/{pid}/pages/1",
                           params={"layout_id": pl["layout_id"], "pair": 1})
        check("PDF 双页跨页", ppair.status_code == 200 and len(ppair.content) > 5000,
              f"{len(ppair.content)} bytes")
        pcover = client.get(f"{BASE}/books/{pid}/cover", params={"w": 240})
        check("PDF 封面（首页缩略）", pcover.status_code == 200, f"{len(pcover.content)} bytes")

        # ------------------------------------------------------------ FB2
        print("== FB2 ==", flush=True)
        fid = find_book(client, "/测试库/FB2", "示例fb2.fb2")
        fmeta = client.get(f"{BASE}/books/{fid}/meta").json()
        check("FB2 元数据", fmeta["mode"] == "flow", f"format={fmeta['format']}")
        check("FB2 标题", fmeta["title"] == "示例 FB2", fmeta["title"])
        check("FB2 章节数 = 3", len(fmeta["chapters"]) == 3, f"{len(fmeta['chapters'])}")
        fl = client.post(f"{BASE}/books/{fid}/layout", json=PROFILE).json()
        check("FB2 排版", fl["total_pages"] >= 3, f"pages={fl['total_pages']}")
        fpage = client.get(f"{BASE}/books/{fid}/pages/1",
                           params={"layout_id": fl["layout_id"]})
        (OUT / "fb2_page_1.png").write_bytes(fpage.content)
        check("FB2 渲染第 1 页", fpage.status_code == 200 and len(fpage.content) > 3000,
              f"{len(fpage.content)} bytes")

    print()
    if failures:
        print(f"FAILED ({len(failures)}): {failures}")
        return 1
    print("ALL FORMAT CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
