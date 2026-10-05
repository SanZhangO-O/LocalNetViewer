# 踩坑记录（LESSONS）

## 2026-10-05 C++ 移植漏掉 Python 的缓存键校验，未鉴权即可任意删除 *.json.gz
- 现象: C++ 版 `GET /books/x/pages/1/meta?layout_id=../../foo`（query 值无任何
  校验）触发 `LayoutsDiskCache::get` 拼出 `cache/layouts/x/../../foo.json.gz`，
  解压/身份校验失败后 `remove()` 该路径；带盘符的分量（`D:/x`）经
  `std::filesystem::operator/` 会整体替换路径。默认配置（有公开书库）未鉴权可达，
  可删任意可读的 `*.json.gz`（含 Python 版共享的 layouts/parsed 缓存）。
- 根因: Python v0.4.1 已修（`util/paths.is_safe_id` + `cache/store.py` 全入口校验），
  C++ 移植时没有对应实现；`layout_id` 是客户端可控输入却只在内存缓存里当字符串用，
  只有磁盘路径会拼接它。
- 修复: `util/paths` 新增 `is_safe_id`（`[A-Za-z0-9_-]{1,64}`），
  `LayoutsDiskCache::get/put`、`ParsedCache::get/put`、`invalidate_book` 前置校验，
  非法值按未命中处理、绝不构造/删除路径。
- 防再犯: 移植安全修复时要"按数据类型"扫全部落点（这里：所有把外部输入拼进文件
  路径的地方），不能只迁移被点名的那个函数；C++/Python 差分审查应把
  Python 的安全工具函数逐个对照是否存在。

## 2026-10-05 Android 一次 /locate 瞬断 = 永久丢阅读位置
- 现象: 打开书时网络抖动，`/locate` 失败被 `getOrDefault(1)` 吞掉 → 定位到第 1 页，
  恢复锚点/比例随即被清空；300ms 后防抖把 page=1 写进本地进度，旧位置永久丢失。
- 根因: "恢复定位失败"被当成"没有进度"处理；且排版中（totalPages=0、currentPage
  还是默认 1）按 Home 时 `saveProgressNow` 无守卫，也会把进度覆盖成第 1 页。
- 修复: locate 失败原样抛出（保留 restoreAnchor，ErrorView 可重试）；
  `saveProgressNow` 在 `layoutId==null || totalPages<=0` 时直接返回。
- 防再犯: 所有"失败回退默认值"的 `getOrDefault` 都要问一句"默认值会不会被当成
  真实数据写回持久层"；进度类状态在"数据未就绪"阶段禁止落盘。

## 2026-10-05 C++ fixed/strip 补超时：worker 不能捕获调用方 adapter
- 现象（修复过程中）: 给 C++ `/layout` 的 fixed/strip 分支补 `run_with_timeout` 时，
  若沿用 Python 的写法（lambda 捕获调用方传入的 `BookAdapter&`），超时返回后路由
  会销毁该 adapter，后台 worker 继续使用即悬垂。
- 根因: C++ `open_adapter` 返回 `unique_ptr`（调用方独占），Python 的适配器生命周期
  较宽松；两边的所有权模型不同。
- 修复: worker 自持 `AppConfig`/`BookRef` 副本并自行 `create_adapter`（与
  `document_internal` 同款），超时后不依赖调用方栈上对象。
- 防再犯: 往 `run_with_timeout` 里放捕获时必须逐个确认捕获对象的生命周期能覆盖
  "超时后台继续跑"；借用引用一律改按值/shared_ptr 或让 worker 自建。

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

## 2026-10-05 C++ 新函数写进 container.cpp 匿名命名空间，声明在头文件却链接失败
- 现象: 在 `container.h` 声明、`container.cpp` 定义的 `lnv::list_dir_image_names`
  编译通过，链接 `lnvserver.exe` 报 `LNK2019 无法解析的外部符号`（services.cpp 引用）。
- 根因: `container.cpp` 从第 287 行起用一个大型匿名命名空间包住所有容器实现，
  到 1169 行才闭合；新函数定义落在这个匿名命名空间内 → 内部链接，头文件声明
  的 `lnv::` 外部符号并不存在。工厂函数（`open_dir` 等）之所以正常，是因为它们
  定义在 1169 行之后。
- 修复: 不导出该内部函数——让 `GalleryService::list_images` 走公开的
  `open_dir(dir, recursive, skip_entries)` + `Container::entries()` 取名称；
  目录枚举缓存留在 `DirContainer` 内部共享。
- 防再犯: 在 `container.cpp` 内新增需被其他 TU 调用的函数时，定义必须放在匿名
  命名空间之外（工厂段之后），或直接复用公开工厂 API；不要只看编译通过就认为
  链接会通过。

## 2026-10-05 删掉"当时以为无用"的 import 导致 /books/open-folder 全 500（图集打不开）
- 现象: 优化图集枚举时把 `server/app/services/gallery.py` 的 `import os` 删掉（以为只
  有被删除的 `os.walk`/`os.scandir` 用它），结果任何 `POST /books/open-folder` 都 500：
  `NameError: name 'os' is not defined`（`save_registry` 里还有 `os.replace`）。
  客户端表现为"图集打不开"。
- 根因: `py_compile` / 导入检查只验证语法，**不验证名字在运行期是否存在**；
  `os.replace` 藏在 `save_registry` 里，删 import 时没被静态检查发现。
- 修复: 恢复 `import os`。用进程内 TestClient 跑 1.5 万张图的 open-folder/meta/layout
  复现并确认 200、耗时 < 0.3s。
- 防再犯: 删除 import 前先全局检索该**模块名下所有属性**的用法（`os.`、`os.replace`…），
  不能只看被删代码块；改完用一次真实调用（哪怕 `python -c` 建临时书库跑 open-folder）
  验证，而不是只依赖 `py_compile`。

## 2026-10-05 漫画"铺满屏幕"的 ≤8% 轻微裁边仍被感知为图片不完整
- 现象: 漫画页宽度占满屏幕，但上下两边内容各被裁掉一点（图片比例与视口接近的页面）；
  真实反馈为"漫画显示出了问题，内容被裁"。
- 根因: `FillScreenScale`（v0.5.1 引入）在覆盖裁边 ≤8% 时返回 `Crop` 缩放，把
  "轻微裁边铺满"当成了可接受代价；对阅读内容而言任何裁切都是丢内容，违背
  "图片完整性优先"的展示要求。
- 修复: 删除 `FillScreenScale`；"铺满屏幕"改为 `ContentScale.Fit`（等比完整显示、
  尽量占满，比例不符只留黑边）；"整页"语义与之相同，选项合并，旧值 `page`
  解析为 `screen`（见 DESIGN.md v0.5.3）。
- 防再犯/回归: `ReaderFitModeDefaultTest` 锁定默认映射与"绝不裁边"（近比例视口断言
  等比缩放后图片不超出视口）、`ReaderFitModeMatrixTest` 锁定模式矩阵；展示图片的
  默认策略不得含任何 `Crop` / `FillBounds` 语义，裁切必须是独立显式模式。

## 2026-10-05 浏览页页码制分页：翻页失败不回退页码 = 指示与列表错位
- 现象（自查发现）: `load()` 发请求时立即把 `page` 置为目标页；同目录翻页失败走
  "保留旧列表 + Snackbar"分支后，页码指示显示第 3 页、列表却还是第 2 页内容。
- 根因: 页码指示（`page`）与列表内容（`entries`）是两个状态，"成功才一起更新"只在
  成功路径成立；失败路径只回滚了 loading，没回滚 page。
- 修复: 同目录翻页失败时把 `page` 回退到请求前的值（`restorePage`；换目录分支列表
  已清空、走整页错误视图，不涉及）。回退后可立即重试失败页（`goToPage` 的
  `target == page` 去重不会再挡路）。
- 防再犯: 状态机里凡是"乐观置位 + 失败保留旧数据"的字段，失败分支必须逐字段确认
  回滚；指示类派生状态要与它描述的数据同源同生命周期。
