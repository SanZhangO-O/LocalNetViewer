# LocalNetViewer 会话规则

- 按 book_id 查书失败时，不得把 `id → vpath` 映射当作书存在的唯一依据：
  文件被移动后索引会残留同指纹（size + 头 1MB）的旧路径条目，必须按同
  book_id 兜底到其余活跃 vpath。记录见 `LESSONS.md` 2026-10-05，
  回归测试 `server/tests/test_cache_invalidation.py::test_book_by_id_falls_back_to_moved_vpath`。
