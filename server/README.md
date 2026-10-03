# LocalNetViewer Server

局域网电子书阅读服务器（设计文档 `../DESIGN.md` 第 4 章）：书库扫描、格式解析、
分页与页面渲染。客户端只按页取图，**整书文件永不传输**。

## 安装

```bat
cd server
python -m venv .venv
.venv\Scripts\activate
pip install -e .
:: 可选：网卡名称识别 / 目录监听 / 二维码
pip install -e ".[sync]"
```

Linux / macOS:

```bash
cd server
python3 -m venv .venv && source .venv/bin/activate
pip install -e .
```

## 配置

编辑 `config.yaml`（关键项）：

| 项 | 说明 |
|---|---|
| `server.host` / `server.port` | 监听地址与端口（默认 `0.0.0.0:8765`） |
| `server.auth_token` | 留空不需要令牌；填写后客户端需带 `X-Auth-Token` |
| `library.roots` | 书库根目录列表，`name` 即虚拟路径第一段 |
| `library.include_ext` | 参与索引的扩展名 |
| `library.encoding_overrides` | TXT 编码检测失败时的书级覆盖 |
| `gallery.folder_as_book` | 是否允许"文件夹作为图集打开" |
| `comic.default_fit` | 漫画默认取页方式（`viewport` / `original`） |
| `render.fonts_dir` | 字体目录（默认 `assets/fonts`） |
| `cache.*` / `limits.*` | 缓存上限与安全上限 |

## 字体（重要）

服务器需要中文字体才能正确渲染中文。请把字体文件放入 `app/assets/fonts/`：

```
app/assets/fonts/NotoSansSC-Regular.otf
app/assets/fonts/NotoSansSC-Bold.otf      # 可选
```

未提供时服务器会退回系统字体（Windows 的微软雅黑 / Linux 的 Noto CJK / macOS 的
PingFang），并在启动信息中给出告警；再找不到任何字体时中文会显示为方框。

## 启动

```bat
python run.py --config config.yaml
```

启动信息示例：

```
LocalNetViewer Server v0.1
书库: 我的书库 (D:/Books, 123 本)  [扫描完成]
字体: NotoSansSC-Regular（3 个可用）
令牌: 未启用

可用地址:
  http://192.168.1.10:8765   (以太网（推荐）)
  http://10.8.0.3:8765       (虚拟网卡（VPN，客户端可能不可达）)

Windows 首次运行若手机连不上，请以管理员执行：
  netsh advfirewall firewall add rule name="LocalNetViewer 8765" dir=in action=allow protocol=TCP localport=8765
```

常用参数：`--host` / `--port` 覆盖监听、`--no-scan` 跳过启动扫描、
`--log-level debug` 打开调试日志、`--reload` 开发热重载。

## 接口

Base URL `http://<host>:8765/api/v1`，交互式文档见 `http://<host>:8765/docs`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/server/info` | 服务信息（无需令牌，客户端据此判断是否需要令牌） |
| GET | `/files?path=/&limit=1000&offset=0` | 目录列表（含图集入口标记） |
| GET | `/books/{id}/meta` | 元数据 + 章节目录 + 封面 URL |
| GET | `/books/{id}/cover?w=240` | 封面图 |
| POST | `/books/{id}/layout` | 注册排版参数 → `layout_id`、总页数、章节起始页 |
| GET | `/books/{id}/pages/{n}` | 页面内容：`format=png`（默认）/ `json`，固定版式支持 `fit`/`quality`/`pair` |
| GET | `/books/{id}/pages/{n}/meta` | 页元数据（锚点、章节名） |
| GET | `/books/{id}/locate` | 锚点 → 页码 |
| GET | `/books/{id}/search?q=` | 全文搜索（TXT/EPUB） |
| GET | `/books/{id}/asset?path=` | 书内资源（EPUB 图片） |
| POST | `/books/open-folder` | 文件夹 → 虚拟图集书 |
| GET | `/render/fonts` | 可用字体列表 |
| GET | `/render/fonts/{id}/file` | 字体文件下载 |
| GET/PUT | `/progress/{id}?device_id=` | 服务器端进度同步（可选） |

## 目录结构

```
server/
├─ pyproject.toml
├─ config.yaml
├─ run.py                  # 启动入口
└─ app/
   ├─ main.py              # FastAPI 装配、路由注册、生命周期
   ├─ context.py           # 依赖容器（服务/缓存/字体装配）
   ├─ config.py            # 配置加载与校验
   ├─ netinfo.py           # 本机地址枚举（多网卡/VPN 标注）
   ├─ api/                 # 路由：server / files / books / read / progress / errors
   ├─ services/            # library / gallery / layouts / search / progress
   ├─ formats/             # base / txt / epub / pdf / comics / fb2 / mobi / registry
   ├─ layout/              # engine（分页）/ fonts（宽度表）/ anchors（锚点换算）
   ├─ render/              # raster（Pillow 栅格化）/ jsonout（JSON 模式）
   ├─ cache/               # 解析 / 布局 / 页面 三级缓存
   ├─ models/schemas.py    # HTTP 契约（Pydantic DTO）
   ├─ util/                # 路径安全 / 自然排序 / 文本 / 图像 / 并发 / XML
   └─ assets/fonts/        # 内置字体放置处
```

## 运行期目录（`cache/`）

| 路径 | 内容 |
|---|---|
| `cache/index.json` | 书库索引（book_id 指纹、目录图片数） |
| `cache/parsed/` | 解析缓存（DocumentModel，gzip JSON，按配额 LRU） |
| `cache/layouts/{book_id}/` | 布局缓存（LayoutProfile + PageMap，重启后 layout_id 仍有效） |
| `cache/galleries.json` | 文件夹图集虚拟书注册表 |
| `cache/converted/` | MOBI/AZW3 经 Calibre 转换出的 EPUB |
| `cache/progress.json` | 服务器端进度同步（可选功能） |

页面图片只缓存在内存（LRU，字节 + 张数双上限），客户端 Coil 磁盘缓存兜底。

## 说明

- 单进程部署（`workers=1`），内存缓存天然一致；渲染在 `asyncio.to_thread` 线程池执行。
- 只读服务：除可选的进度同步外没有写入端点；路径穿越、zip-slip、图片炸弹均有防护。
- 设计文档第 11 章列出的可选后端（CBR 需 `unrar`/`bsdtar`，CB7 需 `py7zr`，
  MOBI/AZW3 需 Calibre）在缺失时不影响其它格式，列表会把对应条目标记为不可打开。
