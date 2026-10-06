# 踩坑记录（LESSONS）

## 2026-10-05 Android 一次 /locate 瞬断 = 永久丢阅读位置
- 现象: 打开书时网络抖动，`/locate` 失败被 `getOrDefault(1)` 吞掉 → 定位到第 1 页，
  恢复锚点/比例随即被清空；300ms 后防抖把 page=1 写进本地进度，旧位置永久丢失。
- 根因: "恢复定位失败"被当成"没有进度"处理；且排版中（totalPages=0、currentPage
  还是默认 1）按 Home 时 `saveProgressNow` 无守卫，也会把进度覆盖成第 1 页。
- 修复: locate 失败原样抛出（保留 restoreAnchor，ErrorView 可重试）；
  `saveProgressNow` 在 `layoutId==null || totalPages<=0` 时直接返回。
- 防再犯: 所有"失败回退默认值"的 `getOrDefault` 都要问一句"默认值会不会被当成
  真实数据写回持久层"；进度类状态在"数据未就绪"阶段禁止落盘。

## 2026-10-05 文件移动后旧索引残留同指纹条目，require_book 永远 404
- 现象: 客户端能看到书（/files 列表正常、book_id 正确），但打开/封面一律 404
  「书不存在或已被移除」；换密码/重启都无效。日志里 `/books/{id}/meta -> 404 1ms`。
- 根因: 文件从 `/书库/X.epub` 移动到 `/书库/子目录/X.epub` 后，lazy 模式不会重扫，
  `cache/index.json` 同时残留两条**同 book_id** 条目（指纹 = size + 头 1MB，移动后不变）。
  `load_index` 构建 `id → vpath` 时同指纹后写者覆盖，键序（逐字节排序）决定
  **失效旧路径胜出** → `require_book` 永远解析到已不存在的旧路径 → 404。
  而列表按 vpath 查 `by_vpath_`，所以"能看到、打不开"（`services/library.py::book_by_id`）。
- 修复: require_book / book_by_id 增加**同指纹 vpath 兜底**——主 `id → vpath`
  解析失效（不存在/越界）时，在索引里找同 book_id 的其余 vpath 逐个尝试，
  命中后把 `id → vpath` 晋升到活跃路径（Python `_ref_from_vpath`）。
- 验证: 用真实毒索引（server/cache/index.json）复现 404 → 修复后 meta/cover 200
  （`verify_pyfix.py`）。
  排查手段：在 require_book 各失败分支临时打点重建，`NOT_FILE` 打印出
  `F:\book\[Kmoe]...epub`（缺子目录段）直接暴露旧路径。
- 防再犯: `id → vpath` 只是缓存加速，**不得作为书存在的唯一依据**；按 id 查书
  失败时必须考虑"同指纹已移动"的情形。已由真实索引回归验证覆盖。

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

## 2026-10-06 py7zr 1.x 移除 read()/readall()，CB7 列表正常但取页一律 422
- 现象: CB7 能列出（/files、meta、layout、contents 全 200，页数正确），但
  `/pages/{n}` 一律 422 PARSE_FAILED：`7z 条目读取失败 p01.png:
  'SevenZipFile' object has no attribute 'read'`（page meta 的 aspect 也因此 0.0）。
- 根因: `formats/comics.py::SevenZipComicContainer._read_by_real` 调用
  `SevenZipFile.read(targets=[...])`——这是 py7zr 0.x 的内存读取 API；
  环境（服务器 venv）装的是 py7zr 1.1.3，1.x 删除了 `read()`/`readall()`，
  只保留 `extract(path/targets/factory=WriterFactory)`。
- 修复（已应用）: `_read_by_real` 按 py7zr 版本分流——有 `read()` 的 0.x 走原路径；
  1.x 走 `extract(targets=[real], factory=py7zr.io.BytesIOFactory(limit))` 后
  `factory.get(real).read()`，`limit` 取 `limits.max_archive_entry_mb`（0 则 512MB 兜底），
  超限仍由 `_check_size` 读后校验。回归测试 `tests/test_containers.py::
  test_sevenzip_container_reads_entries`（真实 7z 写入 3 图 → 容器 read + 适配器
  page_bytes/aspect 全链路）。
- 防再犯: 可选依赖升级（py7zr/rarfile/ebooklib）后在真格式文件上跑一遍
  "列出 → 打开 → 取页/取资源"三步验证，不能只看 `import` 成功。

## 2026-10-06 基类抽 `_read_by_real` 后漏改 EpubImageContainer → 图片型 EPUB/MOBI 漫画构造即崩
- 现象: 全量 pytest 6 个失败（test_epub_comic / test_mobi_comic / test_access /
  test_api）。图片型 EPUB 一打开就
  `TypeError: Can't instantiate abstract class EpubImageContainer without an
  implementation for abstract method '_read_by_real'`——不仅测试挂，
  "图片型 EPUB 自动按漫画打开"运行时也是坏的（meta/layout/pages 全不可用）。
- 根因: `ComicContainer.read()/read_entry()` 重构为统一走抽象 `_read_by_real`
  （流式限流入口）时，zip/rar/7z/tar/folder 五个容器都补了实现，
  唯独 `EpubImageContainer` 还在直接覆盖 `read()`、也没实现新抽象方法——
  ABC 在实例化时才报错，而这几个测试在更早的行就崩了，后面的断言从未跑过，
  连带掩盖了夹具与断言的错位。
- 修复: `EpubImageContainer` 实现 `_read_by_real`（转 `epub._read_entry`），并在
  构造时填 `_raw_entries`（保持 spine 顺序、尺寸取 `epub.entry_size`）+
  `_finish_init()`，基类 `read_entry/asset` 随之可用；`names` 仍按 spine 顺序。
- 连带发现（陈旧断言，一并修正）: ①两个漫画测试的夹具 `_page_image(i)` 从
  i=1 起算，断言却按 i=0 写（page1 期望 200x300，实际 210x310）——修好前置
  崩溃后才暴露；②test_access 两处断言与已文档化契约矛盾（错误令牌在任何端点
  统一 401；全锁定时输入某书库密码只可见对应书库）；③test_api 期望
  show_all_files=false 时普通文件列出，与其自身注释"隐藏"矛盾。
- 防再犯: 给抽象基类加抽象方法时，全局检索**所有子类**（含测试里直接构造的）；
  测试夹具与断言的数值必须同源；"测试在更早的行失败"不代表后面的断言正确，
  修好前置崩溃后要重看全部断言。
