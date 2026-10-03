"""通过真实 HTTP 走一遍完整阅读闭环，并保存渲染出的页面图作为证据。

用法（服务器需已在 8765 运行）：
    .venv\\Scripts\\python.exe -X utf8 ..\\.e2e\\smoke_http.py
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
    "margin": {"left": 40, "right": 40, "top": 40, "bottom": 180},
    "font": {"family": "auto", "size_px": 52, "line_height": 1.6},
    "paragraph": {"spacing_px": 20, "first_line_indent_chars": 2},
    "image": {"max_width_ratio": 1.0, "max_height_ratio": 1.0},
    "chapter_new_page": True,
    "mode": "flow",
}

failures: list[str] = []


def check(label: str, ok: bool, detail: str = "") -> None:
    mark = "OK  " if ok else "FAIL"
    print(f"[{mark}] {label} {detail}")
    if not ok:
        failures.append(label)


def book_id(client: httpx.Client, folder: str, name: str) -> str:
    data = client.get(f"{BASE}/files", params={"path": folder}).json()
    for entry in data["entries"]:
        if entry["name"] == name:
            return entry["book_id"]
    raise AssertionError(f"{name} 不在 {folder} 里")


def main() -> int:
    with httpx.Client(timeout=60.0) as client:
        # ---------------------------------------------------------- 目录
        roots = client.get(f"{BASE}/files", params={"path": "/"}).json()
        check("书库根列表", any(e["name"] == "测试库" for e in roots["entries"]))

        # ---------------------------------------------------------- TXT
        bid = book_id(client, "/测试库/小说", "长篇小说.txt")
        meta = client.get(f"{BASE}/books/{bid}/meta").json()
        check("TXT 元数据", meta["mode"] == "flow" and len(meta["chapters"]) == 8,
              f"章节 {len(meta['chapters'])}")
        check("TXT 章节标题", meta["chapters"][0]["title"].startswith("第1章"),
              meta["chapters"][0]["title"])

        layout = client.post(f"{BASE}/books/{bid}/layout", json=PROFILE).json()
        layout_id, total = layout["layout_id"], layout["total_pages"]
        check("TXT 排版", total > 10, f"layout={layout_id} pages={total}")

        # 每页锚点 → locate 必须回到原页，且章节与 meta 一致
        round_trip_ok = True
        chapter_ok = True
        for page in (1, 2, total // 2, total):
            m = client.get(f"{BASE}/books/{bid}/pages/{page}/meta",
                           params={"layout_id": layout_id}).json()
            loc = client.get(f"{BASE}/books/{bid}/locate", params={
                "layout_id": layout_id,
                "chapter": m["anchor"]["chapter"],
                "block": m["anchor"]["block"],
                "char": m["anchor"]["char"],
            }).json()
            if loc["page"] != page or loc["clamped"]:
                round_trip_ok = False
            if loc["chapter_index"] != m["chapter_index"]:
                chapter_ok = False
        check("锚点往返", round_trip_ok)
        check("locate 与 meta 章节一致", chapter_ok)

        # 渲染几页并存图
        for page in (1, total // 2):
            resp = client.get(f"{BASE}/books/{bid}/pages/{page}",
                              params={"layout_id": layout_id, "theme": "paper"})
            ext = "png" if "png" in resp.headers["content-type"] else "jpg"
            (OUT / f"txt_page_{page}.{ext}").write_bytes(resp.content)
            check(f"渲染 TXT 第 {page} 页", resp.status_code == 200 and len(resp.content) > 2000,
                  f"{len(resp.content)} bytes, etag={resp.headers['etag']}")

        # 换字号后按锚点恢复
        m5 = client.get(f"{BASE}/books/{bid}/pages/5/meta",
                        params={"layout_id": layout_id}).json()
        big = dict(PROFILE)
        big["font"] = {"family": "auto", "size_px": 76, "line_height": 1.8}
        layout2 = client.post(f"{BASE}/books/{bid}/layout", json=big).json()
        loc2 = client.get(f"{BASE}/books/{bid}/locate", params={
            "layout_id": layout2["layout_id"],
            "chapter": m5["anchor"]["chapter"],
            "block": m5["anchor"]["block"],
            "char": m5["anchor"]["char"],
        }).json()
        check("改字号后锚点仍可定位",
              layout2["layout_id"] != layout_id and not loc2["clamped"],
              f"新页数 {layout2['total_pages']}，锚点落在第 {loc2['page']} 页")

        # 搜索
        search = client.get(f"{BASE}/books/{bid}/search",
                            params={"q": "第3章", "layout_id": layout_id}).json()
        check("全文搜索", search["total"] >= 1 and search["results"][0]["page"],
              f"命中 {search['total']} 条，首页码 {search['results'][0]['page'] if search['results'] else None}")

        # ---------------------------------------------------- GB18030
        gbid = book_id(client, "/测试库/小说", "gb18030编码.txt")
        gmeta = client.get(f"{BASE}/books/{gbid}/meta").json()
        check("GB18030 编码识别", gmeta["chapters"][0]["title"].startswith("第1章"),
              gmeta["chapters"][0]["title"])

        # ---------------------------------------------------------- 漫画
        cbid = book_id(client, "/测试库/漫画", "示例漫画.cbz")
        cmeta = client.get(f"{BASE}/books/{cbid}/meta").json()
        check("CBZ 元数据", cmeta["mode"] == "fixed" and cmeta["page_count"] == 12,
              f"页数 {cmeta['page_count']}")
        clayout = client.post(f"{BASE}/books/{cbid}/layout",
                              json={**PROFILE, "mode": "fixed"}).json()
        check("CBZ 排版", clayout["total_pages"] == 12)
        cpage = client.get(f"{BASE}/books/{cbid}/pages/3",
                           params={"layout_id": clayout["layout_id"], "fit": "viewport"})
        (OUT / "comic_page_3.jpg").write_bytes(cpage.content)
        check("渲染漫画第 3 页", cpage.status_code == 200 and len(cpage.content) > 1000,
              f"{len(cpage.content)} bytes")
        cmeta_page = client.get(f"{BASE}/books/{cbid}/pages/3/meta",
                                params={"layout_id": clayout["layout_id"]}).json()
        check("漫画页高宽比", abs((cmeta_page["aspect"] or 0) - 1.5) < 0.05,
              f"aspect={cmeta_page['aspect']}")

        pair = client.get(f"{BASE}/books/{cbid}/pages/1",
                          params={"layout_id": clayout["layout_id"], "pair": 1})
        check("双页跨页渲染", pair.status_code == 200 and len(pair.content) > 1000,
              f"{len(pair.content)} bytes")

        original = client.get(f"{BASE}/books/{cbid}/pages/1",
                              params={"layout_id": clayout["layout_id"], "fit": "original"})
        check("原图模式 + Range",
              original.status_code == 200 and original.headers.get("accept-ranges") == "bytes")
        ranged = client.get(f"{BASE}/books/{cbid}/pages/1",
                            params={"layout_id": clayout["layout_id"], "fit": "original"},
                            headers={"Range": "bytes=0-99"})
        check("Range 分块", ranged.status_code == 206 and len(ranged.content) == 100)

        # ---------------------------------------------------------- 图集
        folder = client.post(f"{BASE}/books/open-folder",
                             json={"path": "/测试库/图集"}).json()
        check("文件夹注册为图集", folder["page_count"] == 6, f"book_id={folder['book_id']}")
        gmeta2 = client.get(f"{BASE}/books/{folder['book_id']}/meta")
        check("图集可打开", gmeta2.status_code == 200)
        glayout = client.post(f"{BASE}/books/{folder['book_id']}/layout",
                              json={**PROFILE, "mode": "fixed"}).json()
        gpage = client.get(f"{BASE}/books/{folder['book_id']}/pages/1",
                           params={"layout_id": glayout["layout_id"]})
        check("渲染图集第 1 页", gpage.status_code == 200 and len(gpage.content) > 1000,
              f"{len(gpage.content)} bytes")
        cover = client.get(f"{BASE}/books/{folder['book_id']}/cover", params={"w": 240})
        check("图集封面", cover.status_code == 200 and cover.headers["content-type"].startswith("image/"))

        # 单张图片
        solo = client.post(f"{BASE}/books/open-folder",
                           json={"path": "/测试库/图集/img_01.png"})
        check("单张图片注册", solo.status_code == 200, solo.text[:120])
        if solo.status_code == 200:
            sid = solo.json()["book_id"]
            slayout = client.post(f"{BASE}/books/{sid}/layout",
                                  json={**PROFILE, "mode": "fixed"}).json()
            spage = client.get(f"{BASE}/books/{sid}/pages/1",
                               params={"layout_id": slayout["layout_id"]})
            check("单张图片可渲染", spage.status_code == 200,
                  f"pages={slayout['total_pages']}")

        # ---------------------------------------------------------- 封面
        txt_cover = client.get(f"{BASE}/books/{bid}/cover", params={"w": 240})
        (OUT / "txt_cover.jpg").write_bytes(txt_cover.content)
        check("TXT 文字封面", txt_cover.status_code == 200 and len(txt_cover.content) > 500,
              f"{len(txt_cover.content)} bytes")
        # 重启后配色不能变 —— 同一进程内两次请求必须一致
        again = client.get(f"{BASE}/books/{bid}/cover", params={"w": 240})
        check("封面可复现", again.content == txt_cover.content)

        # ---------------------------------------------------------- 进度
        put = client.put(f"{BASE}/progress/{bid}", params={"device_id": "e2e"},
                         json={"anchor": {"chapter": 2, "block": 0, "char": 0}, "page": 9, "ratio": 0.3})
        got = client.get(f"{BASE}/progress/{bid}", params={"device_id": "e2e"})
        check("进度写入/读取", put.status_code == 200 and got.json()["page"] == 9)

    print()
    if failures:
        print(f"FAILED: {len(failures)} -> {failures}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
