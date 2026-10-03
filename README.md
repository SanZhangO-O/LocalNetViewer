# LocalNetViewer

局域网电子书阅读系统：**PC 上的 Python 服务器负责书库管理、格式解析、分页与页面渲染；
Android 客户端通过 IP 连接服务器，浏览文件并"逐页"阅读电子书与漫画，书籍不整本下载。**

完整设计见 [DESIGN.md](DESIGN.md)（v0.3）。实现状态、验证证据与设计对照见
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)。

```
LocalNetViewer/
├─ DESIGN.md                   # 设计文档（唯一权威规格）
├─ IMPLEMENTATION_STATUS.md    # 实现状态 / 与设计的差异 / 验证证据 / 已知限制
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
:: 把中文字体放到 server\app\assets\fonts\NotoSansSC-Regular.otf（见该目录 README）
:: 编辑 config.yaml，把 library.roots 指向你的书库目录
python run.py --config config.yaml
```

启动信息会列出所有可用地址（多网卡 / VPN 会标注类型），例如：

```
LocalNetViewer Server v0.1
书库: 我的书库 (D:/Books, 123 本)  [扫描完成]

可用地址:
  http://192.168.1.10:8765   (以太网（推荐））
```

交互式接口文档：`http://<服务器IP>:8765/docs`。

### 2. Android 客户端

用 Android Studio 打开 `android/`，或命令行构建：

```bat
cd android
gradlew.bat assembleDebug
```

安装后进入连接页，输入服务器启动信息里给出的 `IP:端口`（可省略端口，默认 8765），
如服务器启用了令牌则一并填写。

## 核心约束与设计取舍

- **整书文件永不传输**：客户端只按页取 PNG/JPEG，页码与排版全部由服务器决定
  （避免"服务器分页 vs 客户端字体差异"）。
- **阅读位置用锚点 `{chapter, block, char}` 而不是页码**：改字号、旋转屏幕后由服务器
  把锚点换算成新页码（`/locate`），实现"重新布局不失位"。
- **三级缓存**：解析缓存（磁盘）/ 布局缓存（内存 + 磁盘，重启后 `layout_id` 仍有效）/
  页面缓存（内存 LRU，字节 + 张数双上限），客户端 Coil 磁盘缓存兜底。
- **服务器渲染、客户端控制**：页码 / 章节名不由服务器绘制，客户端叠加半透明底栏，
  服务器通过 `margin.bottom` 预留空间，因此切换沉浸模式不会触发重新排版。

## 已实现的范围（里程碑）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 骨架与连通 | FastAPI 骨架、`/server/info`、`/files`、配置加载、统一错误；客户端连接页 + 浏览页 | 已实现 |
| M1 TXT 阅读闭环 | TXT 适配器、DocumentModel、分页引擎、PNG 渲染、`/layout`、`/pages`、页元数据、`/locate`；客户端 Pager、进度记忆、预取 | 已实现 |
| M2 EPUB + 阅读设置 | EPUB 适配器（meta 快速返回 + 首次布局全量解析并缓存）、内嵌图片、封面、`/render/fonts`；目录抽屉、设置面板（字号/行距/边距/主题/字体） | 已实现 |
| M3 漫画与固定版式 + 搜索 | PDF 直渲、漫画容器（CBZ/CBR/CB7/CBT/图片文件夹）、文件夹图集虚拟书、`fit=viewport\|original`、全文搜索；客户端漫画控制、图集入口、搜索结果跳页 | 已实现 |
| M4 可选增强 | 服务器端 JSON 模式、条漫切片、服务器端进度同步已实现；客户端 JSON 渲染、mDNS、扫码、TTS 未实现 | 部分实现 |

## 服务器支持的格式

| 格式 | 说明 |
|---|---|
| TXT | 编码检测（BOM → charset-normalizer → GB18030）+ 章节切分，可配置书级编码覆盖 |
| EPUB | ebooklib 解析，缺失/异常时自动退回 zipfile + lxml 手写 OPF/spine 解析 |
| PDF | pypdfium2 直渲（固定版式） |
| CBZ/CBR/CB7/CBT、图片文件夹 | 图片即页，自然排序；CBR 需 `unrar`/`bsdtar`，CB7 需 `py7zr` |
| FB2 | XML → 块模型 |
| MOBI/AZW3 | 检测到 Calibre `ebook-convert` 时转换缓存为 EPUB（可选） |

## 测试与验证

服务器已带 pytest 测试套件（锚点往返、编码检测、zip-slip / 路径穿越、缓存失效、容器名称映射、
中文字体探测、并发工具、HTTP 契约与端到端 API），客户端已通过 Gradle 构建验证：

```bat
cd server
python -m venv .venv
.venv\Scripts\python -m pip install -e .
.venv\Scripts\python -m pytest -q

cd ..\android
gradlew.bat assembleDebug
```

`.e2e/` 下是端到端验证脚手架（不属于发布内容）：

```bat
cd server
.venv\Scripts\python -X utf8 ..\.e2e\setup_e2e.py            :: 生成测试书库与配置
.venv\Scripts\python -X utf8 run.py --config ..\.e2e\server-config.yaml
.venv\Scripts\python -X utf8 ..\.e2e\smoke_http.py           :: 真实 HTTP 全链路
.venv\Scripts\python -X utf8 ..\.e2e\android_e2e.py          :: 模拟器 UI 全流程
```

详细验证证据与已知限制见 [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) 第 0/3 节，
缺陷修正记录见 [DESIGN.md](DESIGN.md) 第 13 节。

> **中文字体**：服务器会优先挑选系统里真正含中文字形的字体（Windows 的微软雅黑 / 等线、
> macOS 的苹方等），因此开箱即可显示中文。若要固定观感，把 Noto Sans SC 放到
> `server/app/assets/fonts/`（见该目录 README），它会被优先选中。

端到端联调清单（真机手测）：打开书 → 连续翻页 → 改字号 → 横竖屏 → 断网重连 →
服务器重启后继续阅读 → 文件夹作为图集打开 → 漫画方向/适应模式 → 搜索跳页。
