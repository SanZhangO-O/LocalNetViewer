"""为端到端验证准备书库与服务器配置（写入 .e2e/）。

生成：
  .e2e/library/小说/长篇小说.txt        多章 TXT（GB18030 与 UTF-8 各一份）
  .e2e/library/漫画/示例漫画.cbz        12 页 CBZ
  .e2e/library/图集/                    6 张图片的文件夹（"作为图集打开"）
  .e2e/server-config.yaml               指向该库的服务器配置
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
LIB = ROOT / "library"


def _chapter(title: str, paragraphs: int = 26) -> str:
    lines = [title, ""]
    for i in range(paragraphs):
        lines.append(
            f"　　这是{title}的第 {i + 1} 段正文。服务器负责分页与渲染，"
            "客户端只按页取图；改字号或旋转屏幕后由锚点换算回原来的位置。"
        )
        lines.append("")
    return "\n".join(lines)


def write_txt() -> None:
    folder = LIB / "小说"
    folder.mkdir(parents=True, exist_ok=True)
    text = "\n".join(_chapter(f"第{i + 1}章 测试章节") for i in range(8))
    (folder / "长篇小说.txt").write_text(text, encoding="utf-8")
    # 故意用 GB18030 编码一份，验证编码检测
    (folder / "gb18030编码.txt").write_text(
        "\n".join(_chapter(f"第{i + 1}章 编码测试") for i in range(3)), encoding="gb18030"
    )


def _page(index: int, w: int = 800, h: int = 1200) -> bytes:
    img = Image.new("RGB", (w, h), (250, 248, 245))
    draw = ImageDraw.Draw(img)
    draw.rectangle([20, 20, w - 20, h - 20], outline=(47, 93, 98), width=6)
    draw.rectangle([60, 90, w - 60, 190], fill=(47, 93, 98))
    for row in range(8):
        y = 260 + row * 90
        draw.rectangle([60, y, w - 60 - (row % 3) * 90, y + 34], fill=(190, 200, 200))
    draw.ellipse([w // 2 - 90, h - 260, w // 2 + 90, h - 80], fill=(122, 92, 70))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def write_cbz() -> None:
    folder = LIB / "漫画"
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / "示例漫画.cbz"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for i in range(1, 13):
            zf.writestr(f"page_{i:03d}.png", _page(i))


def write_gallery() -> None:
    folder = LIB / "图集"
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(1, 7):
        (folder / f"img_{i:02d}.png").write_bytes(_page(i, 700, 1000))


def write_config() -> None:
    cfg = f"""# 端到端验证用配置（由 tools/setup_e2e.py 生成，不属于发布内容）
server:
  name: E2E-LocalNetViewer
  host: 0.0.0.0
  port: 8765
  auth_token: ""

library:
  roots:
    - name: 测试库
      path: {LIB.as_posix()}
  include_ext: [".txt", ".epub", ".pdf", ".cbz", ".cbr", ".cb7", ".cbt", ".fb2", ".mobi", ".azw3"]
  show_all_files: false

gallery:
  folder_as_book: true
  min_images_in_folder: 1
  recursive: false
  show_image_files: false

comic:
  skip_entries: ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]
  jpeg_quality: 85
  strip_min_aspect: 1.5

render:
  fonts_dir: {LIB.parent.as_posix()}/fonts
  max_page_pixels: 4000000

cache:
  dir: {LIB.parent.as_posix()}/cache

limits:
  max_book_mb: 512
  parse_timeout_s: 60
"""
    (ROOT / "server-config.yaml").write_text(cfg, encoding="utf-8")


def main() -> None:
    write_txt()
    write_cbz()
    write_gallery()
    write_config()
    for path in sorted(LIB.rglob("*")):
        if path.is_file():
            print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes")
    print("config:", (ROOT / "server-config.yaml"))


if __name__ == "__main__":
    main()
