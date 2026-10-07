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

## 2026-10-07 格式声明分散：`.cbz3` 声明了却打不开，`.rb` 双声明静默覆盖
- 现象: `.cbz3` 在 `registry.py:36` 的 COMIC_EXTS 中，但 `comics._ZIP_EXTS`
  没有它 → 打开 422/415 `UnsupportedFormatError: 不支持的漫画容器: .cbz3`；
  M5 加 Ruby/源码格式时 `.rb` 被 code 与 calibre 两个 spec 同时声明，
  `_EXT_INDEX` 后写覆盖先写，`format_of("x.rb")` 变成 "calibre"（会去跑
  ebook-convert）。
- 根因: 同一格式事实（扩展名 → 容器/适配器/默认列表/客户端清单）分散在
  registry / comics / config.IMAGE_EXT / DEFAULT_INCLUDE_EXT / 客户端硬编码
  等多处，无单一事实源、无一致性校验，必然漂移或静默覆盖。
- 修复: `formats/specs.py` 单一事实源 + `registry` 表驱动（v0.8，DESIGN 7.7）；
  `.cbz3` 并入 zip 容器；`.rb` 归 code（注释说明 Rocket eBook 用户用
  `ext_overrides` 指回 calibre）；`specs._validate()` import 时对重复扩展名/
  非法 mode 直接抛错（快速失败，不再是后写覆盖）。
- 验证: 回归测试 `server/tests/test_format_registry.py`
  （`test_spec_exts_are_reachable` / `test_exts_are_owned_by_single_spec` /
  `test_comic_specs_have_container`）；本次以等价检查脚本实跑
  `ALL REGISTRY CHECKS OK`、新格式冒烟 16/16（按项目约定未跑 pytest 套件）。
- 防再犯: 新增/修改格式只改 `specs.py`；凡"声明支持 X"必须有测试证明从
  列表 → 打开 → 取页全链路通，只改注册表/文档不算数。

## 2026-10-07 压缩包能力判定只认扩展名：嗅探出的 zip 点"查看内容"415
- 现象: `.bin` 内容是 zip（嗅探后列表 `format=zip`、`has_contents=true`），
  点"查看内容"→ 415 `非压缩包格式`（`/books/{id}/contents`）；同一文件
  直接打开（/meta、/layout、/pages）全部正常。
- 根因: `services/archive.py::_container_for` 用 `is_archive_name(ref.name)`
  （纯扩展名）做门禁，而真正打开走 `comics.open_container`（M5 起含魔数
  兜底）→ "能不能浏览"与"能不能打开"两个判定不同源，错扩展名文件被门禁拦下。
- 修复: `_container_for` 改为直接 `open_container` + 检查 `browsable`
  属性，非浏览容器才 415（`services/archive.py`）。
- 验证: HTTP 冒烟：`mystery.bin`（zip 内容）`/contents` 200 列出 `p1.png`
  （见 skills 记录外的临时脚本 `lnv_sniff_smoke.py`，项目内由
  `test_archive_contents.py` 覆盖正常路径）。
- 防再犯: 能力判定与打开路径必须同源：客户端看到的"可浏览/可打开"必须由
  同一函数（`open_container`）产生，禁止复制一份扩展名清单做门禁。

## 2026-10-07 新适配器只做导入检查 → 三个静默退化（ODT 空文本 / format 显示 doc / title_guess 崩溃）
- 现象: 新格式冒烟一次性暴露：①ODT 解析 `chars=0`（内容全丢）；②DOCX/PPTX/
  XLSX 的 `meta.format` 全是 `doc`（本应 docx/pptx/xlsx）；③打开 HTML/Markdown
  抛 `AttributeError: 'MarkdownAdapter' object has no attribute 'title_guess'`。
- 根因: ①`office:text` 的 localname 是 `text`，ODT 递归 walker 的容器白名单
  漏了它，`office:body` 直接往下没有可识别的子元素；②`_ZipXmlAdapter.__init__`
  写 `self.format = ref.format or "doc"`，而普通打开路径 `ref.format` 为空，
  基类覆盖了子类类属性；③HtmlAdapter 直接继承 FlowAdapter，并没有 TxtAdapter
  的 `title_guess` 属性（原实现是给 TXT 系列写的）。
- 修复: officebook walker 白名单加 `text`/`body`；删掉基类 `__init__` 的
  format 赋值（以子类类属性为准）；htmlbook 改用 `stem_of(ref.name)`。
- 验证: 新格式冒烟 16/16（odt `chars=24`、docx/pptx/xlsx format 正确、
  md/html 正常出章）。
- 防再犯: 新适配器写完必须用真实样本跑"meta → layout → page"，只 `import`
  成功/只过 registry 不算通过；基类 `__init__` 不要写会被子类类属性覆盖的
  同名字段（format 这类身份字段应由类属性唯一定义）。

## 2026-10-07 M5 对抗性 review：四处"能力判定不同源"（嗅探/复合扩展名/页白名单/嵌套解析）
- 现象: ①`gallery.show_image_files=false` 失效——已知图片被当"未知扩展名"
  嗅探后重新列出（旧测试 `test_files_hide_single_images_when_disabled` 必挂），
  且 `total` 计入随后被丢弃的未知条目 → 客户端翻到空尾页；②`.fb2.zip` 默认
  既不进索引也不出现在 `/files`（format_of 认得、include_ext 门禁用 `ext_of`
  得到 `.zip` 被排除）；③页面为 TIFF 的既有 CBZ 变 0 页（M5 把 `.tif` 从单图
  清单移除时，容器页白名单复用了同一清单）；④嗅探出的 zip 容器能列 contents、
  能注册嵌套书，但打开嵌套书 404（`ArchiveService.resolve` 仍用
  `is_archive_name` 扩展名门禁）。
- 根因: 同一能力事实存在"多份判据"：列表分类、索引门禁、容器页白名单、
  嵌套 resolve 各自维护扩展名/格式判定；任意一处升级（复合扩展名、TIFF 改为
  多页、嗅探容器）都会让另一处失效——与 2026-10-07 前一天 `.cbz3` 是同一类
  病：能力判定与真正打开不同源。
- 修复: `library` 第一阶段用 `detect`（带缓存）完成未知条目分类，未知二进制
  不再计入 `total`；include_ext/override 比较统一 `ext_key_of`；
  `imaging.page_image_exts()`（单图清单 + TIFF）专供容器/图集页；
  `archive.resolve` 改用 `registry.is_browsable_format`，与 `/contents` 同源。
- 验证: 回归 `server/tests/test_m5_formats.py`
  （`test_fb2_zip_is_listed_by_default`、
  `test_sniffed_entries_respect_listing_contract`、
  `test_container_keeps_tiff_pages`）；本次以等价冒烟实跑通过
  （含错扩展名图片隐藏、total 一致性、CBZ 内 TIFF 计 2 页、fb2.zip 列出）。
- 防再犯: 扩展名比较分三类用途——展示用 `ext_of`；格式/门禁/容器判定一律
  `ext_key_of`（复合扩展名）；"页"白名单单独 `page_image_exts`。任何
  "能不能打开/浏览/算一页"的判定必须调用与真正打开相同的函数或注册表，
  禁止就地复制清单。规则已写入 `AGENTS.md`。

## 2026-10-07 RTF 内置兜底解析：吞字面反斜杠 + O(n²)（2.24MB 需 21s）
- 现象: striprtf 未安装时（默认路径），正文 `C:\new\share` 解析成 `C:`；
  构造 2.24MB 的 CJK RTF（每个汉字一个 `\uNNNN`）单次解析 21s，
  `/meta`、`/search` 都会触发。
- 根因: ①主循环已把 `\\` 还原为字面反斜杠、并消费完所有控制字，末尾却又对
  整段结果做 `_CTRL_RE.sub` 等后处理，把"字面反斜杠 + 字母"当残留控制字删除；
  ②每个 `\` 都执行 `re.match(pattern, src[pos:])`，切片复制剩余全文 → O(n²)。
- 修复: 后处理三行删除（循环内已无未消费控制序列）；改用编译后的
  `_UNICODE_AT.match(src, pos)` / `_CTRL_AT.match(src, pos)`（`m.end()` 为
  绝对偏移，直接赋回 pos）。
- 验证: 回归 `server/tests/test_m5_formats.py::test_rtf_keeps_literal_backslash`
  与 `test_rtf_parses_large_input_quickly`；本次实测 2MB 0.14s（原 21s）。
- 防再犯: 流式解析器不要在"已消费"的文本上再做正则清理；正则多分支扫描用
  `pattern.match(text, pos)`，永远不要 `text[pos:]` 切片。
