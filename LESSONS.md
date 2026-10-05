# 踩坑记录（LESSONS）

## 2026-10-05 文件移动后旧索引残留同指纹条目，require_book 永远 404
- 现象: 客户端能看到书（/files 列表正常、book_id 正确），但打开/封面一律 404
  「书不存在或已被移除」；换密码/重启都无效。日志里 `/books/{id}/meta -> 404 1ms`。
- 根因: 文件从 `/书库/X.epub` 移动到 `/书库/子目录/X.epub` 后，lazy 模式不会重扫，
  `cache/index.json` 同时残留两条**同 book_id** 条目（指纹 = size + 头 1MB，移动后不变）。
  `load_index` 构建 `id → vpath` 时同指纹后写者覆盖，键序（逐字节排序）决定
  **失效旧路径胜出** → `require_book` 永远解析到已不存在的旧路径 → 404。
  而列表按 vpath 查 `by_vpath_`，所以"能看到、打不开"。两端同病：C++
  `services/library.cpp::require_book` 与 Python `services/library.py::book_by_id`。
- 修复: require_book / book_by_id 增加**同指纹 vpath 兜底**——主 `id → vpath`
  解析失效（不存在/越界）时，在索引里找同 book_id 的其余 vpath 逐个尝试，
  命中后把 `id → vpath` 晋升到活跃路径。C++ `build_ref_for_vpath` 辅助函数；
  Python `_ref_from_vpath`。
- 验证: 用真实毒索引（server/cache/index.json）复现 404 → 修复后 meta/cover 200
  （C++ `bisect_cache.py` index_only 场景 + Python `verify_pyfix.py`）。
  排查手段：在 require_book 各失败分支临时打点重建，`NOT_FILE` 打印出
  `F:\book\[Kmoe]...epub`（缺子目录段）直接暴露旧路径。
- 防再犯: `id → vpath` 只是缓存加速，**不得作为书存在的唯一依据**；按 id 查书
  失败时必须考虑"同指纹已移动"的情形。已由真实索引回归验证覆盖。
