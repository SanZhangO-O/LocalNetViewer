# LocalNetViewer

局域网电子书阅读系统：**PC 上的 Python 服务器负责书库管理、格式解析、分页与页面渲染；
Android 客户端通过 IP 连接服务器，浏览文件并"逐页"阅读电子书与漫画，书籍不整本下载。**

完整设计见 [DESIGN.md](DESIGN.md)（v0.8）。

```
LocalNetViewer/
├─ DESIGN.md                   # 设计文档（唯一权威规格）
├─ server/                     # Python 服务器（FastAPI + Pillow）+ pytest 测试
├─ android/                    # Android 客户端（Kotlin + Compose）
└─ .e2e/                       # 端到端验证脚手架（不属于发布内容）
```

## 快速开始

### 1. 服务器

```bat
cd server
python -m venv .venv
.venv\Scripts\activate
pip install -e .
copy config.example.yaml config.yaml
:: 把中文字体放到 server\assets\fonts\NotoSansSC-Regular.otf（见该目录 README）
:: 编辑 config.yaml，把 library.roots 指向你的书库目录（对外提供服务时设 host: 0.0.0.0）
python run.py --config config.yaml
```

启动信息会列出所有可用地址（多网卡 / VPN 会标注类型），例如：

```
LocalNetViewer Server v0.8.0
书库: 我的书库 (D:/Books, 123 本)  [扫描完成]

可用地址:
  http://192.168.1.10:8765   (以太网（推荐）)
```

交互式接口文档：`http://<服务器IP>:8765/docs`。

### 2. Android 客户端

前置要求：JDK 17+（首次构建会通过 foojay 解析器自动下载 Gradle 工具链所需的
JDK 21）、首次构建需要联网（下载 Gradle 发行版与依赖）。已配置
`local.properties` 的机器忽略 `sdk.dir` 指向不存在的路径时请删除该文件或修正。

用 Android Studio 打开 `android/`，或命令行构建：

```bat
cd android
gradlew.bat assembleDebug
```

安装后进入连接页，输入服务器启动信息里给出的 `IP:端口`（可省略端口，默认 8765），
按需填写访问密码：`library.roots[].password` 为书库级密码（输入某书库密码只看到该书库
与公开书库），`server.auth_token` 为管理员令牌（看到全部书库）。历史地址按"地址 + 密码"
分别记录，可用不同密码连接同一服务器的不同书库。

> **安全须知**：本项目面向可信局域网。客户端与服务器之间是明文 HTTP（同网络设备
> 可嗅探），访问密码在客户端以明文存储在应用私有目录（已排除云备份与设备迁移），
> 请勿复用重要密码，也不要把服务映射到公网。

## 核心约束与设计取舍

- **整书文件永不传输**：客户端只按页取 PNG/JPEG，页码与排版全部由服务器决定
  （避免"服务器分页 vs 客户端字体差异"）。
- **阅读位置用锚点 `{chapter, block, char}` 而不是页码**：改字号、旋转屏幕后由服务器
  把锚点换算成新页码（`/locate`），实现"重新布局不失位"。
- **三级缓存**：解析缓存（磁盘）/ 布局缓存（内存 + 磁盘，重启后 `layout_id` 仍有效）/
  页面缓存（内存 LRU，字节 + 张数双上限），客户端 Coil 磁盘缓存兜底。
- **服务器渲染、客户端控制**：页码 / 章节名不由服务器绘制，客户端叠加半透明底栏，
  服务器通过 `margin.bottom` 预留空间，因此切换沉浸模式不会触发重新排版。
- **多书库 + 访问密码**：`library.roots` 可配多个书库，每个书库可选 `password`
  （留空 = 公开）；客户端用不同密码连接即进入不同书库，看不到的书库在路径与
  book_id 层面都与"不存在"一致（防跨库探测）。

## 已实现的范围（里程碑）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 骨架与连通 | FastAPI 骨架、`/server/info`、`/files`、配置加载、统一错误；客户端连接页 + 浏览页 | 已实现 |
| M1 TXT 阅读闭环 | TXT 适配器、DocumentModel、分页引擎、PNG 渲染、`/layout`、`/pages`、页元数据、`/locate`；客户端 Pager、进度记忆、预取 | 已实现 |
| M2 EPUB + 阅读设置 | EPUB 适配器（meta 快速返回 + 首次布局全量解析并缓存）、内嵌图片、封面、`/render/fonts`；目录抽屉、设置面板（字号/行距/边距/主题/字体） | 已实现 |
| M3 漫画与固定版式 + 搜索 | PDF 直渲、漫画容器（CBZ/CBR/CB7/CBT/图片文件夹）、文件夹图集虚拟书、`fit=viewport\|original`、全文搜索；客户端漫画控制、图集入口、搜索结果跳页 | 已实现 |
| M4 可选增强 | 服务器端 JSON 模式、条漫切片、服务器端进度同步已实现；客户端 JSON 渲染、mDNS、扫码、TTS 未实现 | 部分实现 |
| M5 格式扩展 | 格式注册表（`formats/specs.py`）+ `/server/formats` 能力接口 + 未知扩展名内容嗅探 + `ext_overrides`；新增 HTML/Markdown/RTF/源码、DOCX/ODT/PPTX/XLSX（内置解析）、旧版 Office（LibreOffice）、多页 TIFF、DjVu、字体预览、ISO/CAB/LHA、HEIC/JXL/SVG/PSD/RAW（可选插件）、音频/视频流式播放 | 已实现 |

## 服务器支持的格式

> M5 起格式清单由 `server/app/formats/specs.py` 单一事实源驱动（30+ 格式、
> 200 个扩展名）；`GET /api/v1/server/formats` 可查询当前服务器实际可用性与
> 置灰原因。可选后端缺失时对应条目在列表置灰，不影响其它格式。

| 类别 | 格式 | 说明 |
|---|---|---|
| 文本 | TXT、源码/结构化文本（py/js/ts/c/cpp/go/rs/json/yaml/toml/sql/css/xml…）、字幕（srt/vtt/ass）、NFO | 编码检测（BOM → charset-normalizer → GB18030）+ 章节切分；代码保留缩进；书级编码覆盖 |
| 网页/排版 | HTML、Markdown、RTF | Markdown 优先 markdown-it-py（可选装 `.[text]`），内置轻量转换器兜底 |
| 电子书 | EPUB、FB2、FBZ/.fb2.zip、MOBI/AZW3 | EPUB 图片型自动按固定版式漫画打开；MOBI 需 Calibre |
| Calibre 族群 | CHM、LIT、PDB、PRC、PML、RB、SNB、TCR、LRF、AZW4、TXTZ、HTMLZ | 经 `ebook-convert` 转 EPUB 后复用解析链（需 Calibre） |
| Office | DOCX、ODT、PPTX、XLSX（内置 zip+XML 解析）；DOC/WPS、XLS、PPT/PPS（需 LibreOffice） | 旧版格式缺失 LibreOffice 时列表置灰 |
| 固定版式 | PDF（pypdfium2 直渲）、DjVu（需 DjVuLibre）、多页 TIFF | 逐页阅读；TIFF 一帧一页 |
| 漫画/压缩包 | CBZ/CBR/CB7/CBT、ZIP/RAR/7Z/TAR（含 .tgz/.tar.gz/.bz2/.xz）、CAB/LHA（bsdtar）、ISO（pycdlib） | 图片即页；压缩包可当虚拟目录浏览，包内书/图片/目录可嵌套打开 |
| 图片 | JPG/PNG/GIF/WebP/AVIF/BMP、TGA/PCX/ICO/DDS/PNM；HEIC/JXL/SVG/PSD/RAW（可选装 `.[images]`） | 单图 = 单页图集；异形格式缺后端时不出现 |
| 字体 | TTF/OTF/TTC | 渲染字样预览页 |
| 媒体 | 音频 MP3/M4A/M4B/AAC/FLAC/OGG/OPUS/WAV/WMA、视频 MP4/M4V/MKV/WEBM/MOV/AVI/3GP | 服务端 Range 流式（206），客户端内嵌播放器，不整段下载 |
| 嗅探兜底 | 任意扩展名 | 无扩展名/错扩展名读文件头判定：文本、图片、文档容器自动可读；`library.sniff_unknown=false` 可关闭；`library.ext_overrides` 可手动指定解析器 |

可选增强后端一次装齐：`pip install -e ".[text,images,iso]"`（Calibre / LibreOffice /
DjVuLibre / unrar / bsdtar 为系统级依赖，启动横幅会列出缺失项与安装提示）。

## 测试与验证

服务器已带 pytest 测试套件（锚点往返、编码检测、zip-slip / 路径穿越、缓存失效、容器名称映射、
中文字体探测、并发工具、压缩包内容浏览与包内嵌套书、HTTP 契约与端到端 API）。
客户端已通过 Gradle 构建与单元测试验证：

```bat
cd server
python -m venv .venv
.venv\Scripts\python -m pip install -e .
.venv\Scripts\python -m pytest -q                      :: Python 服务器

cd ..\android
gradlew.bat assembleDebug
gradlew.bat testDebugUnitTest                          :: 阅读设置/适应模式单元测试
```

`.e2e/` 下是端到端验证脚手架（不属于发布内容）：

```bat
cd server
.venv\Scripts\python -X utf8 ..\.e2e\setup_e2e.py            :: 生成测试书库与配置
.venv\Scripts\python -X utf8 run.py --config ..\.e2e\server-config.yaml
.venv\Scripts\python -X utf8 ..\.e2e\smoke_http.py           :: 真实 HTTP 全链路
.venv\Scripts\python -X utf8 ..\.e2e\android_e2e.py          :: 模拟器 UI 全流程
```

缺陷修正记录见 [DESIGN.md](DESIGN.md) 第 13 节。

> **中文字体**：服务器会优先挑选系统里真正含中文字形的字体（Windows 的微软雅黑 / 等线、
> macOS 的苹方等），因此开箱即可显示中文。若要固定观感，把 Noto Sans SC 放到
> `server/assets/fonts/`（见该目录 README），它会被优先选中。

端到端联调清单（真机手测）：打开书 → 连续翻页 → 改字号 → 横竖屏 → 断网重连 →
服务器重启后继续阅读 → 文件夹作为图集打开 → 漫画方向/适应模式 → 搜索跳页。

## 许可

本项目以 [GPL-3.0-or-later](LICENSE) 许可发布：你可以自由使用、学习、修改和再分发，
再分发时须同样以 GPL-3.0-or-later 许可并保留版权声明，无任何担保。
