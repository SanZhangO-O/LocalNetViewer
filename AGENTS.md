# LocalNetViewer 会话规则

- 按 book_id 查书失败时，不得把 `id → vpath` 映射当作书存在的唯一依据：
  文件被移动后索引会残留同指纹（size + 头 1MB）的旧路径条目，必须按同
  book_id 兜底到其余活跃 vpath。记录见 `LESSONS.md` 2026-10-05，
  回归测试 `server/tests/test_cache_invalidation.py::test_book_by_id_falls_back_to_moved_vpath`。
- 新增/修改文件格式支持时，只改 `server/app/formats/specs.py`（新容器族还需在
  `formats/comics.py` 登记容器扩展名集合）；扩展名跨格式重复会在 import 时抛错。
  记录见 `LESSONS.md` 2026-10-07，回归测试 `server/tests/test_format_registry.py`。
- "可浏览/可打开"能力判定必须与真正打开同源（`comics.open_container`），
  不得另复制一份扩展名清单做门禁。记录见 `LESSONS.md` 2026-10-07。
- 扩展名比较分三类用途：展示用 `ext_of`；格式/门禁/容器/索引判定一律
  `ext_key_of`（复合扩展名如 `.fb2.zip`/`.tar.gz` 必须整体识别）；
  "容器/图集页"白名单单独用 `imaging.page_image_exts()`（含 TIFF）。
  记录见 `LESSONS.md` 2026-10-07，回归测试 `server/tests/test_m5_formats.py`。
