# 实现状态（设计文档 v0.3 ↔ 代码对照）

| 项 | 内容 |
|---|---|
| 设计依据 | [DESIGN.md](DESIGN.md) v0.3（2026-10-03，三轮 review + 首次真实运行验证） |
| 本文用途 | 记录"设计条目 → 代码位置"的映射、相对设计的增量/取舍，以及验证证据与已知限制 |
| 验证状态 | **服务器与客户端均已真实构建、运行并通过端到端验证**（见第 3 节） |

---

## 0. 验证证据（本轮新增）

前三轮 review 都是静态审查；本轮把"从未运行"变成"跑过"。

| 项目 | 命令 | 结果 |
|---|---|---|
| 服务器依赖安装 | `pip install -e .` | 成功（fastapi / Pillow / pypdfium2 / rarfile / py7zr / ebooklib 全部就位） |
| 服务器导入与装配 | `python -c "import app.main; app.main.create_app()"` | 成功，无警告 |
| 服务器测试 | `python -m pytest -q` | **270 项通过 / 1 项跳过**（锚点往返、编码、zip-slip、缓存失效、容器名称映射、中文字体探测、并发工具、HTTP 契约、API 端到端、EPUB/FB2、对抗性回归） |
| 客户端构建 | `gradlew :app:assembleDebug` | 成功（`compileSdk 37`） |
| 客户端静态检查 | `gradlew :app:lintDebug` | **0 error**（9 个刻意保留的告警已在 build.gradle.kts 里注明理由） |
| 服务器静态检查 | `python -m pyflakes app/ tests/` | 干净（无未使用导入 / 未定义名字） |
| 端到端（真实 HTTP） | `.e2e/smoke_http.py` | 27 项检查全通过：目录 → TXT 排版 → 渲染 → 锚点往返 → 改字号恢复 → 搜索 → GB18030 → CBZ → 跨页 → Range → 图集 → 单张图片 → 封面 → 进度 |
| 端到端（EPUB/PDF/FB2） | `.e2e/smoke_formats.py` | 29 项检查全通过：EPUB 元数据/nav 目录/排版/渲染/内嵌图片/封面/搜索/锚点往返；PDF 5 页/原图/Range/跨页/封面；FB2 书名/章节/渲染 |
| 端到端（模拟器 UI） | `.e2e/android_e2e.py` | 全流程通过：连接 → 浏览 → 打开 TXT → 中文渲染 → 滑动翻页 → 工具栏显隐 → 目录高亮 → 设置 → 返回键 → 漫画 |

**只有真实运行才能发现的缺陷**（前三轮静态审查全部漏掉，详见 DESIGN.md 13 节 v0.3）：

| # | 缺陷 | 后果 |
|---|---|---|
| 1 | Pillow 12 的 `getmask()` 返回 `ImagingCore`（没有 `tobytes()`），异常被 `except` 吞掉 | 中文字形探测**恒为 False** → 默认字体退化成 Arial → **整本中文渲染成方框** |
| 2 | `Modifier.transformable` 在 1x 也挂载并消费单指拖动 | **左右滑动永远翻不了页**（点击分区仍有效，静态检查发现不了） |
| 3 | `ReaderScreen.kt` 局部变量前向引用 | **编译失败** |
| 4 | AAR metadata 要求 `compileSdk ≥ 37`，工程写的是 36.1 | **构建失败**（与 #3 是两个独立原因） |
| 5 | 顶栏是半透明叠加层，但 `margin.top` 没预留顶栏高度 | 打开书时**第一行正文被顶栏盖住** |
| 6 | `epub.py` 使用未导入的 `STYLE_NONE` | **任何含内联 `<img>` 的 EPUB** 解析即 NameError → `/layout` 500 |
| 7 | `fb2.py` 把 lxml 元素当真值用（叶子节点为假值） | FB2 书名退化成**文件名** |

另外用 pyflakes 做了一次全量静态扫描（`STYLE_NONE` 这类"名字没定义"只有跑起来才暴露，
扫描可以提前拦住），并用一个自写脚本扫描"把 lxml 元素当真值用"的写法，均已清零。

**第四轮：只针对改动本身的对抗性复核**（差分测试 + 不变量测试）发现 1 个 P1 回归与 6 处边界问题，
已全部修正并补上回归测试（见 DESIGN.md 13 节 v0.3 的"第四轮"小节）。

---
## 1. 里程碑达成情况

### M0 骨架与连通 ✅

| 设计条目 | 代码位置 |
|---|---|
| FastAPI 骨架、路由注册、生命周期 | `server/app/main.py` |
| 配置加载与校验 | `server/app/config.py`、`server/config.yaml` |
| `/server/info` | `server/app/api/routes_server.py` |
| `/files`（目录优先 + 自然排序 + 分页 + gallery 标记） | `server/app/api/routes_files.py`、`server/app/services/library.py` |
| 统一错误模型与异常处理器 | `server/app/api/errors.py`、`server/app/models/schemas.py`（`ErrorCode`） |
| 启动信息（书库数 / 多地址 / 令牌 / 防火墙命令） | `server/app/context.py::startup_report`、`server/app/netinfo.py`、`server/run.py` |
| 客户端连接页（地址 + 令牌 + 历史 + 错误区分） | `android/.../ui/connect/ConnectScreen.kt`、`ConnectViewModel.kt` |
| 客户端浏览页（面包屑、列表、封面） | `android/.../ui/browser/BrowserScreen.kt`、`BrowserViewModel.kt` |
| 页面导航 | `android/.../ui/nav/AppNavHost.kt`（Navigation Compose） |

### M1 TXT 阅读闭环 ✅

| 设计条目 | 代码位置 |
|---|---|
| TXT 适配器（编码检测 + 章节切分 + 64KB 分节兜底） | `server/app/formats/txt.py`、`server/app/util/textutil.py` |
| DocumentModel（章 → 块） | `server/app/formats/base.py` |
| 分页引擎（字符宽度表、贪心折行、超宽单词硬断、避头尾） | `server/app/layout/engine.py` |
| 字体发现与字符宽度表 | `server/app/layout/fonts.py` |
| 锚点 ↔ 页码换算（二分 + `clamped`） | `server/app/layout/anchors.py` |
| PNG 渲染（主题背景 → 文本 → 图片；EXIF 校正；解码防护） | `server/app/render/raster.py`、`server/app/util/imaging.py` |
| `/books/{id}/layout`、`/pages/{n}`、`/pages/{n}/meta`、`/locate` | `server/app/api/routes_read.py` |
| 解析 / 布局 / 页面三级缓存 | `server/app/cache/parsed.py`、`cache/store.py`、`cache/lru.py` |
| 客户端 Pager、翻页、工具栏、进度条、进度记忆、预取 | `android/.../ui/reader/ReaderScreen.kt`、`ReaderViewModel.kt`、`components/ReaderOverlay.kt` |

### M2 EPUB + 阅读设置 ✅

| 设计条目 | 代码位置 |
|---|---|
| EPUB 适配器（meta 容器级快速返回；首次布局全量解析并缓存） | `server/app/formats/epub.py`（`meta()` 只读 spine/toc；`document()` 全量解析） |
| HTML → 块模型（标题/段落/强调/图片/分隔线；忽略 CSS） | `server/app/formats/epub.py::_html_to_blocks` |
| 内嵌图片 `/asset` + 封面提取 | `server/app/formats/epub.py::asset/cover`、`routes_books.py` |
| `/render/fonts`、`/render/fonts/{id}/file` | `server/app/api/routes_server.py` |
| 目录抽屉、设置面板（字号/行距/边距/主题/字体） | `android/.../ui/reader/components/TocSheet.kt`、`SettingsSheet.kt` |
| 改字号 / 横竖屏后位置不失位 | `ReaderViewModel.rebuildLayout()` + `/locate`，300ms 防抖与请求取消 |
| 深色主题 | 服务器 `theme=dark` 预设 + 客户端 `ReaderTheme` 同步配色 |

### M3 漫画与固定版式 + 搜索 ✅

| 设计条目 | 代码位置 |
|---|---|
| PDF 直渲（viewport / original 两种取页） | `server/app/formats/pdf.py` |
| 漫画容器统一抽象（CBZ/CBR/CB7/CBT/图片文件夹）+ 自然排序 + 跳过规则 + 条目名编码兜底 | `server/app/formats/comics.py` |
| 解压后端探测与"不可用置灰" | `server/app/formats/registry.py::backend_status/openable_reason` |
| 文件夹图集虚拟书（`/files` 标记 + `/books/open-folder` + 注册表） | `server/app/services/gallery.py`、`routes_files.py`、`routes_books.py` |
| `fit=viewport\|original`、`quality`、`Range`(206) | `routes_read.py`、`comics.py` |
| 全文搜索（TXT/EPUB，结果含页码） | `server/app/services/search.py`、`routes_read.py` |
| 客户端漫画控制（方向 / 适应模式 / 原图 / 跨页） | `SettingsSheet.kt`、`ReaderSettings.kt`、`PageImage.kt` |
| 客户端"打开为图集"入口（当前目录 + 子目录行） | `BrowserScreen.kt` |

### M4 可选增强 ◐ 部分实现

| 设计条目 | 状态 |
|---|---|
| 服务器 JSON 渲染模式（`format=json`，行级下发 + 图片显示宽高） | ✅ `server/app/render/jsonout.py`、`routes_read.py` |
| 条漫 strip 切片（按视口高度切片，锚点 `(源图索引, 纵向偏移)`） | ✅ `server/app/formats/comics.py::strip_pages/strip_slice_bytes`、`layout/engine.py::build_strip_pagemap`、客户端 `StripReader` |
| 漫画双页跨页（`pair=1` 横向拼接） | ✅ 服务器 + 客户端 `pairPages` 开关 |
| 服务器端进度同步（`/progress/{id}`） | ✅ `server/app/api/routes_progress.py`、`services/progress.py`（键值存储） |
| 客户端 JSON 模式渲染（选词 / TTS） | ❌ 未实现（客户端只用 PNG 模式；服务器已具备 JSON 能力） |
| mDNS 自动发现（`_localnetviewer._tcp`） | ❌ 未实现（手动输入 IP 可用） |
| 控制台二维码 / 扫码连接 | ❌ 未实现 |
| PDF 搜索 | ❌ 未实现（`/search` 对固定版式返回 415 + 明确说明） |
| MOBI/AZW3（Calibre 转换） | ◐ 已实现转换适配器 `server/app/formats/mobi.py`；需用户本机安装 Calibre |

---

## 2. 相对设计文档的增量与取舍

### 2.1 服务器新增模块（设计 4.3 的"规划"之外）

| 新增 | 原因 |
|---|---|
| `app/context.py` | 依赖容器：把配置、字体、三级缓存、服务装配在一起，避免路由 ↔ 服务 ↔ 适配器循环导入（设计 4.3 未列装配层） |
| `app/netinfo.py` | 设计 4.10 要求"打印全部候选地址并标注网卡类型"，需要独立实现（psutil 可选，缺失时退化为标准库枚举） |
| `app/cache/{lru,parsed,store}.py` | 设计 3.4 定义了三级缓存但 4.3 未给出模块，这里独立成包 |
| `app/services/progress.py` | 设计 5.2 #12 的服务器端进度同步需要键值存储 |
| `app/util/{paths,natural,textutil,imaging,concurrency,xmlutil}.py` | 路径穿越防护、自然排序、编码检测、图像防护、超时执行/按键加锁、XML 辅助 |
| `app/api/deps.py` | `AppContext` 注入依赖与可选令牌校验（设计 5.1 的认证约定） |
| `app/formats/mobi.py` | 设计 7.1 的 MOBI/AZW3（P4 可选）适配器 |

### 2.2 接口字段增量（均为向后兼容的**新增**字段）

| 字段 | 位置 | 用途 |
|---|---|---|
| `strip_overlap_px` | 请求体 `LayoutProfile` | 条漫切片重叠像素（设计 2.3 提到"可加重叠像素"），参与 `layout_id` 计算 |
| `strip_min_aspect` | `GET /server/info` → `features` | 客户端据此判断是否提示"条漫模式"（来自 `comic.strip_min_aspect`） |
| `aspect` | `GET /books/{id}/pages/{n}/meta` | 固定版式的页面高/宽比，客户端用于跨页/条漫提示与占位比例 |
| `strip` | `POST /books/{id}/layout` 响应 | 显式告知"当前是条漫切片模式" |
| `libraries` | `GET /files` 根目录响应 | 书库名 + 书籍数（方便浏览页显示） |
| `mode` / `page_count` | `GET /books/{id}/meta` | 固定版式（PDF/漫画/图集）没有章节表，需要直接给出页数 |

### 2.3 配置增量（`config.yaml`）

```yaml
library.encoding_overrides: {}      # 书级编码覆盖（设计 7.2 提到但未给键名）
comic.strip_overlap_px: 0           # 条漫切片重叠像素（内部再限制为 ≤ 视口高度/2）
cache.layout_memory_max: 32         # 内存中保留的 PageMap 数量
cache.layouts_max_mb: 2048          # 磁盘布局缓存上限（设计 4.6 的 LRU 淘汰）
cache.doc_memory_max: 3             # 内存中保留的解析结果数量（渲染页面需要）
limits.max_archive_entry_mb: 512    # 压缩包单条目上限（防 zip 炸弹，设计风险 14）
limits.max_archive_total_mb: 4096   # 压缩包解压总量上限
```

约定：`cache.page_lru_bytes` / `cache.page_lru_pages` 为 0 表示**该维度不限制**；
两项都为 0 表示完全关闭页面缓存（每页重新渲染）。`cache.parsed_max_mb` /
`cache.layouts_max_mb` 为 0 表示磁盘不限制。

### 2.4 关键实现取舍（与设计一致，但落点需说明）

1. **PageMap 只存"页首锚点 + 章节 + 首行"，不存整页行文本**
   渲染时从页首锚点重新排版该页（`FlowLayoutEngine.layout_page`），因此
   "全量分页"与"逐页渲染"共用同一段折行代码，页码与行宽天然一致，且内存占用与
   页数成正比而不是与行数成正比。磁盘格式仍是设计 4.6 的 `cache/layouts/{book_id}/{layout_id}.json.gz`，
   同一文件里额外保存 `LayoutProfile`（`/pages?layout_id=` 只带 id 也能渲染）。
2. **粗体不切换字体文件**：分页宽度与渲染宽度必须一致（设计 3.2 / 风险 16），
   加粗由渲染层二次描边模拟；斜体同理（不换字体）。
3. **CJK 宽度按全角处理**（设计 4.5）；拉丁文本按 token 调用 FreeType 度量并做 memo 缓存，
   避免逐字符 FT 调用。
4. **底栏空间始终预留**：客户端在 `margin.bottom` 上固定加 48dp，
   这样工具栏显隐不会引发重新分页（设计 4.7 的意图 + 6.4 的要求）。
5. **insets 取运行期最大值**：客户端记住常态 WindowInsets 的最大值，
   沉浸模式下 insets 归零也不会触发重排（设计 4.6 / 6.4）。
6. **EPUB 双路径**：主路径 ebooklib，异常/缺失时退回 zipfile + lxml 手写 OPF/spine
   （设计风险 11 的对策），两条路径共用同一 HTML→块转换与资源读取。
7. **`fixed` / `strip` 每次 `/layout` 实时枚举页集合**（设计 4.6），
   并对客户端请求的 `mode` 做能力修正（文本格式强制 `flow`；PDF 请求 `strip` 退化为 `fixed`）。
8. **`layout_id = "L" + sha1(几何 + 解析器版本 + 分页引擎版本 + 已解析字体 id)[:12]`**，
   `theme` / `render_scale` 不参与（设计 4.6）。只哈希几何会留下两个陷阱：解析器升级后
   块索引变化会让旧 PageMap 的锚点指向错误位置（渲染成空白页）；客户端默认
   `family="auto"`，字体目录变化会改变实际字宽。因此把这两项一并纳入哈希。
9. **`book_id` 与虚拟书**：`sha1(size + 头 1MB)[:16]`；图集 `sha1("gallery:" + 虚拟路径)[:16]`。
   由于哈希不可逆，图集注册表持久化在 `cache/galleries.json`（重启后仍能打开）。
10. **Android 手写 DI**（`App.kt` 容器），不引入 Hilt；进度与设置用 DataStore；
    页面图片由 Coil 3（OkHttp fetcher + 磁盘缓存）加载，预取 ±1/±2 页。

### 2.5 Android 依赖版本

`android/gradle/libs.versions.toml` 新增：Retrofit 2.11.0、OkHttp 4.12.0、
retrofit2-kotlinx-serialization-converter 1.0.0、kotlinx-serialization-json 1.7.3、
Coil 3.0.4、navigation-compose 2.8.5、datastore-preferences 1.1.1、
lifecycle-viewmodel-compose / runtime-compose 2.11.0。

**已通过 `gradlew :app:assembleDebug` 验证**（依赖解析、链接、打包全部成功）。构建时发现：

- `compileSdk` 必须 ≥ **37**：`androidx.core 1.19.0` 与 `androidx.lifecycle 2.11.0` 的 AAR metadata
  强制要求，否则 `:app:checkDebugAarMetadata` 直接失败。`targetSdk` 保持 36。
- `material-icons-extended` 钉在 1.7.8（不在所有 Compose BOM 里），与 BOM 管理的
  compose-ui 1.11.0 / foundation 1.10.4 / material3 1.4.0 可以共存。
- `Icons.Filled.List` / `InsertDriveFile` 已弃用，改用 `Icons.AutoMirrored.Filled.*`。

---

## 3. 验证方式与已知限制

### 3.1 如何复现验证

```bat
:: 服务器
cd server
python -m venv .venv
.venv\Scripts\python -m pip install -e .
.venv\Scripts\python -m pytest -q

:: 客户端
cd android
gradlew.bat assembleDebug

:: 端到端（宿主机服务器 + Android 模拟器）
cd server
.venv\Scripts\python -X utf8 ..\.e2e\setup_e2e.py
.venv\Scripts\python -X utf8 run.py --config ..\.e2e\server-config.yaml
.venv\Scripts\python -X utf8 ..\.e2e\smoke_http.py      :: 真实 HTTP 全链路
.venv\Scripts\python -X utf8 ..\.e2e\android_e2e.py     :: 模拟器 UI 全流程
```

`.e2e/` 下是验证脚手架（书库生成、HTTP 冒烟、UI 驱动）与截图，不属于发布内容。

### 3.2 仍需人工确认的事项

| 项 | 说明 |
|---|---|
| 真机（非模拟器）表现 | 目前只在 x86_64 模拟器上验证过；真机的字体目录、内存压力、手势手感需实测 |
| CBR / CB7 实际解压 | 本机没有 `unrar`/`bsdtar` 后端，RAR 路径用**替身模块**做了名称映射与错误路径的回归测试，未跑过真实 RAR |
| MOBI / AZW3 转换 | 本机装了 Calibre，转换路径已实现并发保护与原子落盘，但未在端到端流程里跑过 |
| 条漫 strip 模式的真机体验 | 服务端切片逻辑有测试覆盖；客户端纵向连续滚动的长列表表现需实测 |
| 云备份规则 | 已排除含令牌的设置存储；实际备份/恢复行为需在真机上确认 |
| R8 / release 构建 | `isMinifyEnabled` 仍为 `false`（见 3.3） |

### 3.3 明确未做（设计已列为 P4 或本轮有意延后）

| 项 | 状态 |
|---|---|
| `strings.xml` 全量外提 / i18n | **未做**：UI 字符串仍硬编码在 Kotlin 里 |
| 客户端 JSON 渲染模式 | **未做**：服务器已实现 `format=json`（并修正了重复缩进问题），客户端只走 PNG |
| 客户端接入服务器端进度同步 | **未做**：服务器已实现 per-device LWW，客户端仍只用本地进度 |
| mDNS 发现 / 扫码配对 | **未做**：需手动输入 `IP:端口` |
| TTS / 全文替换 / 笔记 | **未做**（设计 1.3 明确 out of scope） |
| release 开启 R8 / 资源压缩 | **未做**：无法在无真机的情况下验证混淆后的序列化与 Retrofit 行为，宁可不缩 |
| 条漫"按需解码窗口" | **未做**：见 DESIGN.md 13 节"仍然存在的已知限制" |

### 3.4 已识别的风险（沿用设计第 11 章）

风险清单与对策见 [DESIGN.md](DESIGN.md) 第 11 章。本轮新增的两条实际命中：

- 风险 11（EPUB 解析兜底）与风险 15（解析超时）—— 超时机制原先**不覆盖分页本身**，已补；
- 风险 17（漫画手势冲突）—— 真实表现为"滑动完全翻不了页"，已修并写入 6.4 的实现约束。

---
## 4. 历史静态审查记录（前三轮的只读审查，保留备查）

两位只读审查代理分别通读了 `server/`（45 个模块）与 `android/`（22 个 Kotlin 文件 + 构建脚本）
并给出结论。以下为**已修复**的真实缺陷（均为阅读代码发现，不是猜测）：

| # | 严重度 | 位置 | 问题 | 修复 |
|---|---|---|---|---|
| 1 | BLOCKER | `formats/pdf.py` | 读了不存在的配置键 `limits.max_page_pixels`（实际在 `render` 段）→ 任何 PDF 打开即 `AttributeError` | 改为 `env.config.render.max_page_pixels` |
| 2 | BUG | `render/raster.py` | 固定版式缩放被乘两次（`viewport_w` 已含 scale，又传 `render_scale=scale`）→ `scale=2.0` 实际渲染 4 倍，且单页像素上限失效 | `fixed_page_bytes` / `strip_page_bytes` 传 `render_scale=1.0` |
| 3 | BUG | `render/raster.py` | 无内嵌封面的书（TXT/FB2）生成文字封面时调用残留名 `imaging_default_font` → `NameError` | 改为模块内的 `default_font` |
| 4 | BUG | `layout/fonts.py` | 文本宽度缓存用 `hash(text)` 作键，32 位截断会碰撞并返回**错误行宽**，破坏折行与页码 | 键改为文本本身（仅缓存 ≤64 字符的短串） |
| 5 | BUG | `formats/txt.py` | 文件首行即章节标题时产生一个空的首章，目录整体偏移一位 | `flush()` 守卫改为"没有正文且没有标题就不产出章节" |
| 6 | BUG | `formats/comics.py` | ZIP 条目尺寸表用原始名、读取用 `normpath` 名 → `_guard_entry` 查不到尺寸，zip 炸弹防护失效 | 尺寸表与映射表同键（单趟构建） |
| 7 | BUG | `ui/reader/ReaderViewModel.kt` | 空书（0 页）时客户端永远停在加载态 | 显式提示"没有可显示的内容" |
| 8 | BLOCKER(编译) | `ui/{connect,browser,reader}/*Screen.kt` | 参数名 `viewModel` 遮蔽 `viewModel()` 函数 → 无法调用 | 参数改名 `overrideViewModel` |
| 9 | 改进 | `layout/engine.py` | 超长块（整本不换行的 TXT）单次 tokenize 全段，内存/时间尖峰 | 超过 20 万字符分段折行，锚点偏移保持连续 |
| 10 | 改进 | `formats/txt.py` | 母本全角段首缩进与排版缩进叠加成 4 个全角空格 | 解析时剥掉行首缩进，统一由 `first_line_indent_chars` 给出 |
| 11 | 改进 | `api/routes_read.py` | 条漫页元数据用切片序号取源图长宽比（页号 ≠ 源图序号） | 用 `record.image_index + 1` |
| 12 | 改进 | `ui/common/ComposeUtils.kt` | 组合后用 `LaunchedEffect` 回写 insets 状态，多触发一轮重组 | 改为组合期取最大值的普通持有对象 |
| 13 | 改进 | `AndroidManifest` / Gradle | `material-icons-extended` 依赖 BOM 提供版本，个别 BOM 未收录该 artifact | 显式钉 `1.7.8` |
| 14 | 改进 | `ui/nav/AppNavHost.kt` | `popUpTo(0)` 依赖未文档化的根图 id | 改用 `popUpTo(graph.startDestinationId)` |
| 15 | 改进 | 多处 | `LocalLifecycleOwner` 从 `compose.ui.platform` 迁移到 `lifecycle.compose` | 改用新包名 |
| 16 | 改进 | `api/routes_books.py` | 生成文字封面（Pillow 绘制+编码）在事件循环里同步执行 | 放入 `asyncio.to_thread` |
| 17 | 改进 | `formats/{comics,epub}.py` | 图集容器 `getsize` 未捕获 `OSError`；`xlink:href` 在 HTML 解析器下取不到 | 加保护；按属性名后缀兼容三种写法 |
| 18 | 改进 | `formats/base.py` | `Block.from_dict` 不认设计 4.4 示例里的 `"text"` 简写 | 兼容 `spans` 与 `text` 两种形式 |

审查同时确认：所有跨模块 import 符号都能解析、无导入环、Pydantic v2 API 用法正确、
无冻结 dataclass 被修改、`config.yaml` 与 `AppConfig` 字段一一对应、`/layout` `/pages` `/locate`
`/search` 等调用签名一致、缓存序列化往返一致、**Android DTO 字段名与 `@SerialName`、
以及全部端点路径与查询参数与服务器 `schemas.py` / `routes_*.py` 完全一致**。

审查未覆盖：`ui/reader/components/PageImage.kt` 的手势语义细节、漫画手势仲裁的实机表现、
`proguard-rules.pro`（当前 release 未开启混淆，不影响）。

### 4.2 第二轮审查（分模块深审：语义 / 并发 / 状态机）

第二轮把范围切细（formats / layout+render+cache / api+services / android 逻辑），
只报"符号检查查不出"的问题。以下为已修复项：

**服务器**

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| 1 | `layout/engine.py` | 折行的"行数上限"按 token 数计算：单个超长 token（无空格拉丁文本）会被硬断行时**截断丢字**，剩余正文永远不出现 | 改为"无进展检测"（idx/偏移不再前进才停），保证正文不丢 |
| 2 | `layout/engine.py` | 从段落中间继续排版（翻页）时重新加了段首缩进，每页多出两个全角空格且换行位置与连续排版不一致 | 缩进只在 `offset == 0`（真正块首）时给出 |
| 3 | `layout/engine.py` | 跳过行首空白时把锚点推到空白之后，导致页首锚点大于本页排版起点（文档起始锚点被误报 `clamped`） | 块的第一行保留原始起点作为锚点 |
| 4 | `cache/parsed.py`、`cache/store.py` | 损坏缓存处理漏了 `UnicodeDecodeError`（`ValueError` 子类）→ 该书永久 500，只能手工删文件 | 捕获 `(OSError, ValueError, EOFError, BadGzipFile)` 并隔离 |
| 5 | `cache/store.py` | 加载磁盘布局时不校验 `book_id` / `layout_id`，可能把别的书/别的 layout 的 PageMap 当成有效结果；缺 `end_anchor` 时退化为 `(0,0,0)` 会让所有 `/locate` 报越界 | 校验身份后拒绝；缺 `end_anchor` 时按"最后一页之后"推导 |
| 6 | `services/layouts.py` + `layout_id` | layout 缓存键不含解析器版本与已解析字体 → 升级后旧 PageMap 仍被复用，锚点错位、渲染空白页 | `layout_id` 纳入解析器/引擎版本与字体 id |
| 7 | `cache/store.py` | 磁盘布局缓存**没有淘汰策略**（与设计 4.6 的"LRU 淘汰保证磁盘可控"不符） | 新增 `cache.layouts_max_mb` 配额与 LRU 淘汰 |
| 8 | `services/library.py` | `book_by_id` 不校验索引里的尺寸/mtime：文件被就地替换后旧 `book_id` 仍解析，用旧 PageMap 渲染新内容 | 尺寸/mtime 变化即重算指纹，指纹不同则视为该书不存在 |
| 9 | `services/library.py` | `save_index` 在锁外 `json.dumps` 活字典 → 并发插入时报 `RuntimeError`，`/files` 500 且索引可能写坏丢失 | 快照序列化在锁内，"取快照+落盘"整体串行化 |
| 10 | `services/gallery.py`、`services/progress.py` | 注册表/进度在锁外序列化与读取，并发 `forget`/`put` 会 `KeyError`、覆盖较新数据或写坏文件 | 同一临界区内完成"改内存 + 取快照 + 写盘" |
| 11 | `api/routes_read.py` | 全文搜索在事件循环里直接跑（10MB 文本会卡住所有在途请求） | 移入 `asyncio.to_thread` |
| 12 | `api/routes_read.py` | Range 解析失败也返回 416（RFC 9110 要求忽略无效 Range） | 区分"语法无效 → 忽略返回 200"与"越界 → 416" |
| 13 | `api/routes_read.py` | `If-None-Match` 只做字符串相等比较，不支持 `*`、`W/`、逗号列表；ETag 的 scale 精度与缓存键不一致 | 按 RFC 解析；scale 精度与缓存键统一 |
| 14 | `main.py` | 关停时 `scan_task.cancel()` 停不掉工作线程，与保存索引并发写同一文件；启动时重复扫描两遍 | 扫描改为协作式取消 + `wait_idle` 后再保存；`run.py` 预扫描后不再后台重复扫描 |
| 15 | `main.py`、`services/library.py` | `/`、`/healthz`、`/docs` 在启用令牌时可匿名访问（设计 9 要求所有请求校验） | `/` 与 `/healthz` 加令牌校验；启用令牌时关闭 `/docs`、`/redoc`、`/openapi.json` |
| 16 | `services/library.py` | `show_image_files=true` 且 `min_images_in_folder≥2` 时，图片行谎报"可打开为图集" | 按阈值给出 `openable`/`reason`，并补上确定性 `book_id` |
| 17 | `services/library.py` | `gallery.recursive=true` 时父目录图片数用自身 mtime 校验，子目录变化后长期陈旧 | 递归模式不使用该缓存，直接重算 |
| 18 | `layout/fonts.py` | 字形探测缓存只按字体 id，忽略样例字符（`can_render(family, "😀")` 会返回"中"的结论） | 缓存键含样例字符 |
| 19 | `render/raster.py` | 照片页判定只看面积占比，竖屏下整页照片被误判为文本页而输出 PNG（流量变大） | 增加"无文本行且图片几乎占满宽度"判据；固定版式也套用 `max_page_pixels` |
| 20 | `render/jsonout.py` | JSON 模式未下发段落间距/块位置，客户端逐行绘制会与 PNG 模式纵向错位 | 每个块补 `y`/`height`，页级补 `paragraph` 参数 |

**服务器格式层**

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| 21 | `util/imaging.py` | `probe_size` 不处理 EXIF 方向，而解码路径会做方向校正 → 分页用错宽高、条漫切片错位、图集长宽比误判 | `probe_size` 按 Orientation 交换宽高 |
| 22 | `formats/comics.py` | ZIP 防炸弹只信包头声明尺寸 | 流式读取并强制单条目上限（ZIP/RAR/TAR），7z 做读后校验 |
| 23 | `formats/comics.py` | RAR/7z/TAR 用规范化名去归档里取条目，名称需要规范化的条目必然读失败 | 各容器维护"规范化名 → 归档原始名"映射 |
| 24 | `formats/comics.py` | 条漫切片的 `step` 在 overlap ≥ 视口高时退化成 1px（一张长图切出几万页） | overlap 上限压到视口高度的一半 |
| 25 | `formats/comics.py` | `fit=original` 超出单页字节上限时退化为"视口宽度"，原图模式失去意义 | 退化为**原图宽度**重编码 |
| 26 | `formats/comics.py` | 递归图集扫描不跳过符号链接文件（页数虚高且必然加载失败） | 递归分支同样跳过符号链接 |
| 27 | `formats/fb2.py` | `v`/`text-author`/`date`/`td`/`th` 是直接文本元素却被当容器递归 → 诗句、署名、表格文字整段丢失 | 这些标签按段落输出 |
| 28 | `formats/epub.py` | 有块级子元素的容器被整体跳过，其**直接文本**丢失（`<li>介绍<p>…</p></li>`） | 跳过时仍输出容器的直接文本 |
| 29 | `formats/epub.py` | `data:` URI 图片生成空 asset 的图片块 → 阅读器永久破图 | 直接丢弃该块（封面另有路径） |
| 30 | `formats/txt.py` | meta 只在 >512KB 时切节、document 在 >64KB 就切节 → 目录与实际"第 N 节"数量不一致 | 抽出 `_should_section`，两条路径同判据同边界 |
| 31 | `util/paths.py` | `resolve()` 遇到 NUL 等非法段抛 `ValueError` 未被捕获（本可 400 却成 500） | 一并捕获并转 `PathError` |

**Android**

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| 32 | `ReaderViewModel` | 防抖取消产生的 `CancellationException` 被通用 `catch (Exception)` 吞掉并当"排版失败"展示（连续改字号/旋转屏幕就会闪错） | 先 rethrow `CancellationException` |
| 33 | `ReaderViewModel` | 保存进度时把"当前页"和**上一页的锚点**配对保存 → 翻页后立刻退出会回到更早的页 | 锚点与其所属页一起记录，只有匹配才写锚点，否则只写页码+比例 |
| 34 | `ReaderViewModel` | 恢复锚点在一次成功定位后仍可能再次生效，把读者拉回旧位置 | 恢复锚点只用一次即失效；会话锚点只在"属于当前页"时才可信 |
| 35 | `ReaderScreen` | 双击放大只改了本地标志，从未通知 ViewModel → 只是把 1x 位图放大（文字发虚） | 缩放时调用 `setZoomed()`，以 `scale=2.0` 重新渲染当页 |
| 36 | `PageImage` | 离开当前页后内部缩放状态仍在（回到该页会"看着放大但可滑动"） | 非当前页时复位缩放 |
| 37 | `ReaderScreen` | 沉浸模式隐藏系统栏后，离开阅读页没有恢复 → 浏览页/连接页的栏一直不可见 | `onDispose` 恢复系统栏并清常亮标志 |
| 38 | `ReaderScreen` | 条漫跳转被"首个可见项回写 currentPage"取消，只能滚几屏 | 程序化跳转期间抑制回写，动画结束后恢复 |
| 39 | `BrowserViewModel` | `load()`/`loadMore()` 无取消与路径校验 → 快速连点会显示上一个目录，甚至把旧目录条目追加进新目录 | 保存 Job 并取消；切换目录时丢弃过期响应 |
| 40 | `BrowserViewModel` | `opening` 在协程内才置位，同帧连点会打开两次（返回时又被弹回阅读器） | 同步置位 |
| 41 | `ConnectViewModel` | 连接尝试之间没有取消与序号校验，较早的尝试可能最后改回旧服务器地址 | 单 Job + 序号校验后才写配置与落盘 |
| 42 | `ConnectViewModel` | 主动"断开"后新 ViewModel 会立刻自动重连（连接页无法用于换服务器） | 自动重连每次进程只尝试一次 |
| 43 | `SettingsStore`/`ProgressStore` | DataStore 写失败会以未捕获异常终止协程 | 写入容错并返回内存值 |

**记录在案但有意保留的偏离**

1. **HTTP 层通用 404/405 的错误码**：设计 5.1 的错误码表没有"通用未找到"条目，
   因此框架产生的 404/405 仍使用 `BAD_PATH`/`BAD_REQUEST` 并保留原始 HTTP 状态，
   属于"码与状态不完全对应"的已知偏离（客户端只按 message 提示，不受影响）。
2. **`total_chars` 在 EPUB/FB2 的 meta 阶段为 null**：设计 7.3 明确 meta 只做容器级解析，
   字符数需要全量解析才有；TXT 因为本来就要整篇解码所以给出具体值。
3. **EPUB 章节标题在 meta 阶段用文件名兜底、首次布局后用 `<h1>` 修正**：设计 7.3
   "无 toc 时用 `<h1>/<h2>` 或文件名"允许这一顺序，客户端在 layout 后以返回的章节表为准。
4. **数据缺口**：`library.roots` 未配置时服务器仍可启动，`/files` 会提示根目录不存在。
