# LocalNetViewer C++ 移植规格（PORT_SPEC）

> 面向 C++ 移植者的精确规格。**一切以 `server/app/` 下的 Python 源码为准**（DESIGN.md 仅为背景），本文每节注明来源文件。目标：与 Python 实现二选一可替换——共用 `server/config.yaml`、书库目录、缓存目录，HTTP API 契约完全一致（含 ETag、缓存键、JSON 字节级兼容的落盘格式）。
>
> 版本基线：Python `app.__version__ = "0.1.0"`（来源：`server/app/__init__.py`），`PARSER_VERSION = "1"`（`cache/parsed.py`），`LAYOUT_ENGINE_VERSION = "1"`（`services/layouts.py`），`INDEX_VERSION = 1`（`services/library.py`）。

---

## 目录

1. [HTTP 端点清单](#1-http-端点清单)
2. [config.yaml 与启动行为](#2-configyaml-与启动行为)
3. [数据模型（schemas.py）逐字段表](#3-数据模型-schemaspy-逐字段表)
4. [关键算法细节](#4-关键算法细节)
5. [认证](#5-认证)
6. [缓存目录结构与共用性](#6-缓存目录结构与共用性)
7. [C++ 技术选型建议](#7-c-技术选型建议)
8. [移植风险与可接受的行为差异](#8-移植风险与可接受的行为差异)

---

## 1. HTTP 端点清单

来源：`server/app/main.py`、`server/app/api/routes_*.py`、`server/app/api/deps.py`、`server/app/api/errors.py`、`server/app/models/schemas.py`。

### 1.1 全局约定

- 路由前缀：`/api/v1`（`main.py: APIRouter(prefix="/api/v1")`）。
- 鉴权：除 `GET /api/v1/server/info` 外，全部端点依赖 `require_token`（见 §5）。`GET /`、`GET /healthz`（无前缀）**也**要求令牌（`main.py`）。
- 配置 `auth_token` 非空时，`/docs`、`/redoc`、`/openapi.json` **关闭**（返回 404）。
- 错误响应体（`errors.py::AppError.payload`）：`{"code": str, "message": str}`，`detail` 非空时附加 `"detail": object`。
- 401 时附加响应头 `WWW-Authenticate: X-Auth-Token`。
- 503 SERVER_BUSY 附加 `Retry-After: 5`。

### 1.2 错误码枚举与 HTTP 状态（schemas.py `ErrorCode` / `ERROR_HTTP_STATUS`）

| code | 默认 HTTP | 默认 message（风格） | 触发点 |
|---|---|---|---|
| `BAD_REQUEST` | 400 | "请求参数缺失或非法"（校验）；"format 只支持 png/json"；"fit 只支持 viewport/original"（routes_read） | 参数校验失败、枚举非法 |
| `BAD_PATH` | 400 | "路径非法"/具体原因（`paths.py::PathError` 消息直接透传，如 "path 不允许包含 '..'"、"书库不存在: X"、"路径不存在: X"、"不是目录: X"、"目录无法读取: …"） | normalize_vpath / list_dir |
| `PATH_TRAVERSAL` | 403 | "越出书库根目录: <真实路径>" | resolve_in_root |
| `UNAUTHORIZED` | 401 | "令牌缺失或错误"，detail `{"auth_required": true}` | require_token |
| `BOOK_NOT_FOUND` | 404 | "书不存在或已被移除"，detail `{"book_id": …}` | AppContext.require_book |
| `LAYOUT_NOT_FOUND` | 404 | "layout_id 不存在或已失效，请重新 POST /layout"，detail `{"book_id","layout_id"}` | AppContext.require_layout；search |
| `PAGE_OUT_OF_RANGE` | 404 | `f"页码越界：{page} / {total}"`，detail `{"page","total_pages"}` | get_page / page_meta |
| `ASSET_NOT_FOUND` | 404 | "资源不存在"/具体（"EPUB 内不存在资源: X"、"字体不存在" detail `{"font_id"}`） | adapter.asset、font_file |
| `NOT_FOUND` | 404 | "该书暂无进度记录"，detail `{"book_id"}` | GET progress 无记录 |
| `UNSUPPORTED_FORMAT` | 415 | "格式不支持"/具体（"MOBI/AZW3 需要 Calibre…" reason="需安装 Calibre"、"服务器已关闭"文件夹作为图集"功能"、"该格式暂不支持全文搜索"、`"{fmt} 暂不支持全文搜索"` detail `{"format"}`、固定版式请求 json："固定版式（PDF/漫画/图集）不支持 format=json，请使用 format=png" detail `{"mode"}`） | registry / adapters / routes |
| `PARSE_FAILED` | 422 | "解析失败"/"解析超时"（detail `{"reason":"timeout"}`）、"图片超出像素上限"（detail `{"reason":"image_too_large"}`）、"图片解码失败"（detail `{"reason":"image_decode"}`）、"文件超过上限 XMB"、Calibre 转换失败消息 | ParseError 及其子类（超时/图片）、adapters |
| `SERVER_BUSY` | 503 | "服务器繁忙"/具体："解析正在进行中（同一任务去重），请稍后重试"、`"解析繁忙（并发任务已达上限 8，可能有任务卡住），请稍后重试"`，detail `{"reason":"busy"}` | concurrency._TimedRunner |
| `INTERNAL` | 500 | "服务器内部错误"，detail `{"type": "<异常类名>"}` | 未捕获异常 |

补充映射（`errors.py` Starlette HTTPException 处理器）：404→`BAD_PATH`、401→`UNAUTHORIZED`、403→`PATH_TRAVERSAL`、≥500→`INTERNAL`，其余→`BAD_REQUEST`，message 取 `str(exc.detail)`。

请求校验失败（FastAPI RequestValidationError）→ 400 `BAD_REQUEST`，detail `{"errors": [{"loc","msg","type"}, …]}`（最多 20 条）。

### 1.3 端点明细

所有响应 JSON 字段名与类型精确列出；`?` 后缀表示可空（JSON `null`）。数组用 `[]`。

#### GET /api/v1/server/info（**不鉴权**）—— routes_server.py

响应 `ServerInfoResponse`：
```json
{
  "name": "LocalNetViewer",            // config.server.name 或默认
  "version": "0.1.0",
  "api_version": "v1",
  "auth_required": false,              // bool(config.server.auth_token 非空)
  "libraries": [ {"name": str, "book_count": int, "path": null} ],  // 注意：此处 path 恒为 null（不泄露真实路径）
  "features": {
    "png": true, "json": true, "search": true,
    "gallery": bool(comic.gallery.folder_as_book),
    "fits": ["viewport","original"],   // FeaturesInfo 默认字段（info 端点未覆盖，恒为默认）
    "strip": true, "progress_sync": true,
    "strip_min_aspect": float(comic.strip_min_aspect)
  },
  "server_time": "YYYY-MM-DDTHH:MM:SSZ"   // gmtime 的 strftime
}
```
注意 pydantic alias：Python 字段 `json_mode` 序列化为 `"json"`。

#### GET /api/v1/files（鉴权）—— routes_files.py

Query：`path`（默认 `"/"`，虚拟路径）、`limit`（1..5000，默认 1000）、`offset`（≥0，默认 0）。
响应 `FilesResponse`：
```json
{
  "path": str, "parent": str?, "gallery": {"image_count": int, "can_open_as_book": bool}?,
  "entries": [ {
      "name": str, "type": "dir"|"book"|"file"|"image", "path": str,   // path 为虚拟路径
      "format": str?, "book_id": str?, "size": int?, "mtime": str?,    // mtime 为 ISO8601 Z（iso_time）
      "cover_url": str?, "gallery": {...}?, "openable": bool, "reason": str?
  } ],
  "total": int, "limit": int, "offset": int, "scanning": bool,
  "libraries": [ {"name","book_count","path"}? ]     // 仅 path=="/" 时非 null
}
```
目录列表排序：目录在前、文件在后，各自按 `natural_key(name)`（§4.9）。`path="/"` 时只列书库根（`type:"dir"`，`openable` = 根目录存在）。
副作用：响应前调用 `library.maybe_save()`（索引节流落盘，最短间隔 30s）。

#### GET /api/v1/books/{book_id}/meta（鉴权）—— routes_books.py

响应 `BookMetaResponse`：
```json
{
  "book_id": str, "title": str,       // title = meta.title 或文件名去扩展名
  "author": str?, "format": str, "language": str?, "size": int,
  "mode": "flow"|"fixed"|"strip", "page_count": int?,
  "cover_url": "/api/v1/books/{book_id}/cover",
  "render_support": {"png": true, "json": bool},   // json = adapter.supports_json（flow 格式 true）
  "chapters": [ {"index": int, "title": str, "total_chars": int?, "start_page": int?} ],
  "source": str                        // 虚拟路径
}
```
章节 title 兜底：`f"第 {i+1} 节"`。TXT meta 阶段给出 `total_chars`；EPUB/FB2 的 `total_chars` 为 null，`start_page` 为 null（分页前未知）。

#### GET /api/v1/books/{book_id}/cover（鉴权）—— routes_books.py

Query：`w`（40..1024，默认 240）。
Header：`If-None-Match`。
ETag：`"{book_id}:w{w}"`（带双引号）。命中 → 304。
响应头：`Cache-Control: public, max-age=86400`，`ETag`。
体：图片字节；mime 为适配器返回或文字封面 JPEG（`image/jpeg`，quality 88）。
内存缓存键 `("cover", book_id, w)`，LRU 上限 64MB/256 张（context.py，硬编码）。

#### GET /api/v1/books/{book_id}/asset（鉴权）—— routes_books.py

Query：`path`（必填，容器内相对路径；经 `safe_asset_path` 规范化：拒绝 `..`、NUL、空）。
ETag：`"{book_id}:asset:{stable_hash(safe)}"`，其中 `stable_hash(x) = sha1(utf8(str(x))).hexdigest()[:16]`。
响应头：`Cache-Control: public, max-age=31536000, immutable`，`ETag`。命中 If-None-Match → 304。
体：资源原始字节 + sniff_mime 的 mime。

#### POST /api/v1/books/open-folder（鉴权）—— routes_books.py

Body `OpenFolderRequest`：`{"path": str, "recursive": bool?}`（recursive 缺省用 config.gallery.recursive）。
`gallery.folder_as_book=false` → 415 UNSUPPORTED_FORMAT。
行为：`GalleryService.register`（§4.8）；图片数 < `max(1, min_images_in_folder)` → 415；单图片文件可注册为单页图集（需 min_images ≤ 1）。
响应 `OpenFolderResponse`：`{"book_id","title","format":"gallery","page_count":int,"cover_url","mode":"fixed"}`。

#### POST /api/v1/books/{book_id}/layout（鉴权）—— routes_read.py

Body：`LayoutProfile`（§3），Content-Type JSON。
行为：`effective_profile` 修正 mode（flow 适配器强制 flow；fixed 适配器请求 strip 且非漫画 → fixed；请求 flow → fixed；strip 生效且 `strip_overlap_px==0` 时采用 `comic.strip_overlap_px`）→ `layout_key` 计算 layout_id → flow 走三级缓存，fixed/strip 每次实时枚举页集并存盘。
响应 `LayoutResponse`：
```json
{"layout_id": "L<12位hex>", "mode": "flow"|"fixed"|"strip", "total_pages": int,
 "chapters": [{"index","title","total_chars"?,"start_page"?}], "cache_hit": bool, "strip": bool}
```

#### GET /api/v1/books/{book_id}/pages/{page}（鉴权）—— routes_read.py

Query：
- `layout_id`（必填）
- `format`：`png`（默认）| `json`；其它 → 400
- `theme`：默认 `white`
- `scale`：0.5..2.0，默认 1.0
- `fit`：`viewport`（默认）| `original`；其它 → 400
- `quality`：1..100，缺省用 `comic.jpeg_quality`
- `pair`：0/1（默认 0）；`rtl`：0/1（默认 0）
- Header `If-None-Match`、`Range`

校验顺序：format/fit → book 存在（BOOK_NOT_FOUND 优先）→ layout 存在（LAYOUT_NOT_FOUND）→ 页码范围（PAGE_OUT_OF_RANGE）。

响应头（png 与 json 均带）：
```
ETag: <见 §4.6>
Cache-Control: private, max-age=31536000, immutable
X-Page-Index: <page>
X-Total-Pages: <total>
X-Layout-Id: <layout_id>
```
304：If-None-Match 匹配（支持 `*`、`W/` 前缀、逗号列表，精确字符串比较）→ 304 同头。

- `format=json`：仅 flow 模式；否则 400。响应为 `PageJsonResponse`（§4.7 JSON 模式），不占页面图片缓存。
- `format=png`：
  - flow：`render_flow_page`，mime `image/png` 或 `image/jpeg`（照片页判定）。
  - fixed/strip：`adapter.page_bytes` / `strip_slice_bytes` / `pair_bytes`。
  - 内存缓存键 `page_cache_key`（§4.6）。
  - **Range**：仅 fixed/strip 且 `fit=original` 时支持。先加 `Accept-Ranges: bytes`。单段 `bytes=a-b`（忽略多段，取第一段）、`bytes=-n` 后缀。语法无效 → 忽略按 200 整页；语法有效但越界 → 416 + `Content-Range: bytes */{total}`；成功 → 206 + `Content-Range: bytes {s}-{e}/{total}`。

#### GET /api/v1/books/{book_id}/pages/{page}/meta（鉴权）—— routes_read.py

Query：`layout_id`（必填）。
响应 `PageMetaResponse`：
```json
{"page": int, "total_pages": int,
 "anchor": {"chapter": int, "block": int, "char": int},
 "chapter_index": int, "chapter_title": str, "first_line": str,
 "mode": "flow"|"fixed"|"strip", "aspect": float?}
```
aspect 仅 fixed/strip：调 `adapter.page_aspect(target_page)`（h/w；失败为 null）。strip 模式 `target_page = record.image_index + 1`（页号 ≠ 源图序号）。

#### GET /api/v1/books/{book_id}/locate（鉴权）—— routes_read.py + layout/anchors.py

Query：`layout_id`（必填）、`chapter`（≥0 默认 0）、`block`（≥0 默认 0）、`char`（≥0 默认 0）。
响应 `LocateResponse`：
```json
{"page": int, "total_pages": int, "ratio": float,
 "chapter_index": int, "chapter_title": str,
 "anchor_chapter_index": int?, "clamped": bool}
```
算法见 §4.5。`ratio = clamp((page-0.5)/total, 0, 1)`。

#### GET /api/v1/books/{book_id}/search（鉴权）—— routes_read.py + services/search.py

Query：`q`（必填，min_length=1）、`layout_id`（可选）、`limit`（1..500，默认 50）。
layout 存在但 mode≠flow → 415；adapter 非 flow（按格式）→ 415 detail `{"format"}`。
响应 `SearchResponse`：`{"query": str, "total": int, "results": [{"chapter_index","chapter_title","block","char","page":int?,"snippet"}], "limited": bool}`。行为见 §4.10。

#### GET /api/v1/progress/{book_id}（鉴权）—— routes_progress.py

Query：`device_id`（可选）。书必须存在（BOOK_NOT_FOUND）。指定 device 无记录且无任何记录 → 404 `NOT_FOUND`；未指定 device 时返回该书 `updated_at_ts` 最新一条。
响应 `ProgressRecord`：`{"book_id","device_id"?,"anchor":{chapter,block,char},"page":int?,"ratio":float?,"updated_at":"…Z"}`。

#### PUT /api/v1/progress/{book_id}（鉴权）

Query：`device_id`（可选）。Body `ProgressPutRequest`：`{"anchor":{chapter,block,char},"page":int?,"ratio":float?}`，ratio 写入前 clamp 到 [0,1]。
存储：`cache/progress.json`，键 `"{book_id}:{device_id}"`（device 规范化见 §4.10），最多 5000 条（按 `updated_at_ts` 淘汰最旧），写入节流 ≥1s、关停 flush。响应同 ProgressRecord（内部多存 `updated_at_ts` float，不外发）。

#### GET /api/v1/render/fonts（鉴权）—— routes_server.py

响应 `FontsResponse`：`{"default": str, "fonts": [{"id","name","file_name","default":bool,"builtin":bool}]}`。
`id` = 字体文件 stem；`builtin` = 来自 `render.fonts_dir`。

#### GET /api/v1/render/fonts/{font_id}/file（鉴权）

不存在 → 404 ASSET_NOT_FOUND。响应：字体文件字节，mime `.otf/.otc` → `font/otf`，否则 `font/ttf`；带 `filename`（Content-Disposition）与 `Cache-Control: public, max-age=31536000, immutable`。

#### GET / 与 GET /healthz（鉴权）

`/` → `{"name","version","api":"/api/v1","auth_required"}`；`/healthz` → `{"ok": true, "scanning": bool}`。

---

## 2. config.yaml 与启动行为

来源：`server/app/config.py`、`server/run.py`、`server/config.yaml`。

### 2.1 全部键、默认值、语义

相对路径一律相对 **config.yaml 所在目录**（`base_dir`）解析。config 文件不存在时使用内置默认值启动（无书库）。

**server**
| 键 | 默认 | 语义 |
|---|---|---|
| `name` | `"LocalNetViewer"` | 展示名（/server/info、/） |
| `host` | `"127.0.0.1"` | 监听地址；安全默认仅本机 |
| `port` | `8765` | 1..65535 |
| `auth_token` | `""` | 空=不鉴权；非空=所有端点（除 /server/info）需 X-Auth-Token |

**library**
| 键 | 默认 | 语义 |
|---|---|---|
| `roots` | `[]` | `{name, path}` 列表；name 不得含 `/`、`\`，不得重名（校验失败启动即报错）；path 做 `expanduser`/`expandvars` |
| `include_ext` | `.txt .epub .pdf .cbz .cbr .cb7 .cbt .fb2 .mobi .azw3` | 列表中显示的书籍扩展名（自动补 `.`、小写、去重） |
| `show_all_files` | `false` | 列出非电子书（`type:"file"`，`openable:false`，reason "非电子书格式"） |
| `sort` | `"natural"` | 仅声明；实际列表排序硬编码 natural |
| `encoding_overrides` | `{}` | 虚拟路径/文件名/后缀 → 编码名（TXT 书级覆盖；匹配顺序：vpath 精确 → 文件名精确 → endswith 遍历） |

**gallery**
| 键 | 默认 | 语义 |
|---|---|---|
| `folder_as_book` | `true` | 允许把目录注册为图集书 |
| `min_images_in_folder` | `1` | 目录可作图集的最少图片数 |
| `recursive` | `false` | 图集是否递归含子目录 |
| `show_image_files` | `false` | 目录列表是否列出单张图片文件（type:"image"） |

**comic**
| 键 | 默认 | 语义 |
|---|---|---|
| `skip_entries` | `["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]` | 容器/目录跳过 glob（fnmatch，匹配规范化路径或 basename；`*/.*` 额外对 basename 生效） |
| `default_fit` | `"viewport"` | 声明性默认（路由默认值硬编码 viewport） |
| `jpeg_quality` | `85` | 重编码 JPEG 质量（1..100） |
| `strip_min_aspect` | `1.5` | h/w 超过视为长图 |
| `strip_overlap_px` | `0` | 条漫切片重叠像素（profile.strip_overlap_px=0 时采用） |

**render**
| 键 | 默认 | 语义 |
|---|---|---|
| `fonts_dir` | `"assets/fonts"` | 内置字体目录（builtin 优先于系统字体） |
| `default_font` | `"NotoSansSC-Regular"` | 默认字体 id/name（不区分大小写匹配） |
| `max_page_pixels` | `4000000` | 单页渲染像素上限（≥10000）；超出自动降 scale |
| `jpeg_for_photo_pages` | `true` | 照片页用 JPEG q85 |

**cache**
| 键 | 默认 | 语义 |
|---|---|---|
| `dir` | `"cache"` | 缓存根目录 |
| `parsed_max_mb` | `4096` | parsed 磁盘配额（0=不限；超限按 mtime LRU 删到 80%） |
| `page_lru_bytes` | `536870912` | 页面内存缓存字节上限（0=不限） |
| `page_lru_pages` | `512` | 页面内存缓存张数上限 |
| `layout_memory_max` | `32` | 内存 PageMap 条数 |
| `layouts_max_mb` | `2048` | layouts 磁盘配额（0=不限；超限 mtime LRU 删到 80%，可 keep 当前文件） |
| `doc_memory_max` | `3` | 内存 DocumentModel 条数 |

注意：`page_lru_bytes` 与 `page_lru_pages` **同时为 0 = 完全关闭**页面缓存；单条超过字节上限的直接拒绝缓存（不能解释为无限）。

**limits**
| 键 | 默认 | 语义 |
|---|---|---|
| `max_book_mb` | `2048` | 单本书大小上限（0=不限）；registry.create_adapter 与 TXT/FB2 内部均校验 |
| `max_page_bytes` | `52428800` | fit=original 原图直通字节上限；超出强制缩放重编码（quality 提到 comic.jpeg_quality） |
| `max_source_pixels` | `30000000` | 源图解码像素上限（Pillow MAX_IMAGE_PIXELS 语义） |
| `parse_timeout_s` | `60.0` | 解析/分页超时（>0） |
| `max_archive_entry_mb` | `512` | 压缩包单条目解压上限（流式强制，读 limit+1 字节判超） |
| `max_archive_total_mb` | `4096` | 压缩包图片条目总解压上限（开包时按声明尺寸求和校验） |

### 2.2 启动流程与 CLI（run.py）

- CLI：`--config`（默认 `server/config.yaml`）、`--host`（覆盖 cfg）、`--port`（覆盖 cfg）、`--no-scan`（跳过启动同步扫描；注意 Python 实现 `create_app(background_scan=bool(args.no_scan))` 使 `--no-scan` 同时关掉后台扫描——移植时保留该行为或在文档中标注差异）、`--reload`（开发用，忽略 --config）、`--log-level`（critical/error/warning/info/debug/trace）。
- 启动顺序：加载配置（失败 exit 2）→ 建 AppContext（ensure_dirs：cache、cache/parsed、cache/layouts、fonts_dir；`Image.MAX_IMAGE_PIXELS = limits.max_source_pixels`）→ 读 `cache/index.json` → 非 `--no-scan` 时**同步**全量扫描一次（增量：size+mtime+format 未变则复用索引中的 book_id）→ 打印横幅（书库统计、字体、令牌状态、全部网卡地址、防火墙提示）→ uvicorn 单进程（workers=1）启动。
- 横幅安全告警：host 非回环且未配令牌时打印醒目警告。
- 关停：cancel 扫描 → wait_idle(10s) → save_index → progress flush。
- 访问日志中间件：`METHOD path -> status 耗时ms`。
- 地址枚举（netinfo.py）：优先 psutil，退化用 getaddrinfo + UDP connect(8.8.8.8) 探测出口；虚拟网卡（vpn/tap/tun/docker/wsl 等关键字）标记"可能不可达"；IPv6、回环、链路本地不推荐。

---

## 3. 数据模型（schemas.py）逐字段表

来源：`server/app/models/schemas.py`（pydantic v2；所有默认值即缺省请求体行为）。

### 3.1 LayoutProfile（POST /layout 请求体；全部物理像素）

| 模型.字段 | 类型 | 默认 | 约束 |
|---|---|---|---|
| viewport.w | int | 必填 | ≥1 |
| viewport.h | int | 必填 | ≥1 |
| margin.left/right/top/bottom | int | 0 | ≥0 |
| font.family | str | `"auto"` | — |
| font.size_px | int | 52 | 8..400 |
| font.line_height | float | 1.6 | 1.0..4.0 |
| font.letter_spacing_px | float | 0.0 | −10..40 |
| paragraph.spacing_px | float | 20.0 | 0..400 |
| paragraph.first_line_indent_chars | int | 2 | 0..8 |
| image.max_width_ratio | float | 1.0 | >0..2 |
| image.max_height_ratio | float | 1.0 | >0..2 |
| chapter_new_page | bool | true | — |
| mode | "flow"\|"fixed"\|"strip" | `"flow"` | — |
| strip_overlap_px | int | 0 | 0..2000 |

派生：`avail_width = max(1, w − left − right)`；`avail_height = max(1, h − top − bottom)`。
`geometry_dict()`（参与 layout_id 的字段，**theme/scale 不参与**）：viewport `[w,h]`；margin `[l,r,t,b]`；font `[family, size_px, round(lh,4), round(ls,4)]`；paragraph `[round(spacing,4), indent]`；image `[round(wr,4), round(hr,4)]`；`chapter_new_page`；`mode`；`strip_overlap_px`。

### 3.2 其余 DTO

- `Anchor`：`chapter:int=0, block:int=0, char:int=0`。
- `ChapterInfo`：`index:int, title:str, total_chars:int?, start_page:int?`。
- `RenderSupport`：`png:bool=true, json:bool=false`（Python 字段名 `json_mode`，alias 输出 `json`）。
- `BookMetaResponse`：见 §1.3。
- `LayoutResponse`：`layout_id, mode, total_pages, chapters[ChapterInfo], cache_hit=false, strip=false`。
- `PageMetaResponse` / `LocateResponse` / `PageJsonResponse`：见 §1.3 / §4.7。
- `SearchItem`：`chapter_index, chapter_title, block, char, page:int?, snippet`；`SearchResponse`：`query, total, results, limited=false`。
- `GalleryInfo`：`image_count=0, can_open_as_book=false`。
- `FileEntry` / `FilesResponse` / `LibraryInfo` / `FeaturesInfo`：见 §1.3。
- `OpenFolderRequest`：`path:str, recursive:bool?`；`OpenFolderResponse`：`book_id, title, format="gallery", page_count, cover_url?, mode="fixed"`。
- `FontInfo`：`id, name, file_name, default=false, builtin=false`；`FontsResponse`：`default, fonts[]`。
- `ProgressRecord`：`book_id, device_id?, anchor, page?, ratio?, updated_at`；`ProgressPutRequest`：`anchor, page?, ratio?`（ratio clamp [0,1]）。
- `ErrorBody`：`code, message, detail?`。

### 3.3 内部模型（formats/base.py、layout/engine.py，落盘格式）

- `Span{text, style}`，style ∈ `none|bold|italic|bold_italic`；序列化时 `none` 省略。
- `Block{type, spans[], level=1, asset="", w=0, h=0}`；type ∈ `heading|paragraph|image|separator`。`to_dict`：heading 输出 `{type,level,spans}`；paragraph `{type,spans}`；image `{type,asset,w,h}`；separator 只 `{type}`。`from_dict` 兼容 `{"type","text"}` 简写。
- `Chapter{index, title, blocks[]}`；`total_chars = Σ len(block.text)`（仅 heading/paragraph）。
- `DocumentModel{book_id, format, meta:dict, chapters[]}`；`to_dict` 顺序固定：`book_id, format, meta, chapters`。
- `PageRecord{page(1基), anchor:[c,b,ch], chapter_index, chapter_title, first_line, image_index=-1, y0=0, height=0}`；to_dict 键序：`page, anchor, chapter_index, chapter_title, first_line, image_index, y0, height`。
- `PageMap{book_id, layout_id, mode, total_pages, pages[], chapters[], end_anchor:[c,b,ch], created_at:float}`；`from_dict` 兼容缺 end_anchor（按最后页 anchor 推导 `(c, b+1, 0)`）、缺 total_pages（=len(pages)）。

---

## 4. 关键算法细节

### 4.1 book_id 生成（services/library.py `compute_book_id`）

```
book_id = sha1( ascii(str(int(file_size))) + 文件头前 1 MiB 原始字节 ).hexdigest() 的前 16 个 hex 字符
```
- 输入：文件大小十进制 ASCII 字符串（无分隔）+ 头部 `1<<20` 字节（读失败则跳过，不报错）。
- 输出：16 个小写 hex 字符。内容指纹：移动/改名不失效；size 或 mtime 变化即重算；同 book_id 但 size/mtime 与索引不符时会重算指纹，指纹变化 → 旧 id 作废并清空该书全部派生缓存（layout/parsed/page/cover，见 context.py `invalidate_book_caches`）；指纹相同（1MB 之后的等长编辑）也强制失效缓存但 id 不变。

### 4.2 TXT（formats/txt.py、util/textutil.py）

**编码检测链 `decode_text(data, override)`**：
1. 若配置 `encoding_overrides` 命中该 vpath/文件名 → 首选该编码。
2. `detect_encoding(data)`：
   a. BOM 表（按序）：`EF BB BF`→utf-8-sig；`FF FE 00 00` / `00 00 FE FF`→utf-32（codecs 消费 BOM）；`FF FE` / `FE FF`→utf-16。
   b. 无 BOM UTF-16 启发：取前 8192 字节（偶数长度），偶/奇位 0x00 占比 ≥0.2 → 候选 utf-16-le/be，试解并要求控制字符占比 <2%（中文 UTF-16 无 BOM 不猜）。
   c. charset-normalizer `from_bytes`：取 chaos 最小；**仅当** chaos 打平的候选同时含韩文族（cp949/euc_kr/johab/uhc/ks_c_5601/korean）与中文族（gb18030/gbk/gb2312/cp936/big5/big5hkscs/cp950/hz，别名归族映射如 cp932→shift_jis）时改选中文编码；否则取排序第一。
   d. 纯 ASCII 可解 → `ascii`；否则 **GB18030 兜底**。
3. 逐候选尝试解码（override → detect → `utf-8, gb18030, big5, shift_jis, cp1252`），失败降级；正文残留 U+FEFF 一律 lstrip。全部失败 → `utf-8/replace` 解码。

**行/章节**：
- 行切分正则：`\r\n|\r|\n`。
- `_clean_line`：`\t`→4 空格、rstrip、lstrip 全角空格+半角空格+tab（缩进统一交给排版层）。
- 章节正则（整行 strip 后匹配，行长 >60 字符不算标题）：
  1. `^\s*第[零一二三四五六七八九十百千万两0-9]{1,10}[章卷回节部篇集]\s*.{0,30}$`
  2. `^\s*Chapter\s+\d+.{0,30}$`（IGNORECASE）
  3. `^\s*(序章|序言|前言|引子|楔子|后记|尾声|番外|附录).{0,20}$`
- meta 与 document 必须用同一判据：无任何标题命中且 (UTF-8 字节数 > 512KB) 且 (总字符 > 64KB) 时，按"整行字符数累计 ≥64*1024 起一节"切为"第 N 节"。
- meta `language`：前 20000 字符中 CJK（U+4E00-9FFF / U+3040-30FF）占比 ≥5% → `"zh"`。

### 4.3 EPUB / FB2 / 漫画 / PDF / MOBI

**EPUB**（formats/epub.py）：
- 主路径 ebooklib，失败（缺失/不合规）退化 zipfile+lxml 手写：`META-INF/container.xml` → rootfile `full-path` → OPF；manifest(id→href/media-type/properties) + spine(itemref 顺序，仅 `application/xhtml*`)；章标题：EPUB3 nav（properties 含 `nav`）优先，其次 NCX（navPoint/navLabel/text + content/src），href 去 fragment、unquote、相对 OPF 目录规范化。
- meta 快速：只解析容器（spine+标题），不解析正文 HTML。
- document：按 spine 顺序 HTML→块；忽略 CSS；跳过 `script style head title nav noscript template iframe object video audio math`（含祖先在跳过集合内的元素）；`h1..h6`→heading（level 1..6；含块级后代时只取直接文本）；文本容器 `p div li blockquote pre td th figcaption dd dt section article main aside header footer caption address`→paragraph（含块级后代时只取直接文本，顺序在子块之前）；`hr`→separator；`img/image`（src/data-src/xlink:href/Clark 记法/属性名后缀 `href`）→image 块（`data:` URI 丢弃；尺寸 probe 失败默认 600×800）。空白折叠 `[ \t\r\n\f\v]+`→单空格；CJK 相邻空格删除（正则 `(?<=[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef])[ \t]+(?=同左)`）；b/strong→bold，i/em→italic（span 合并相邻同 style）。章 title：toc 标题 → 首个非空 heading → 文件名 stem。
- 资源读取：依次尝试原名/unquote/去前导`/`/normpath/小写全名匹配/basename 唯一匹配（多个同名 → ASSET_NOT_FOUND）；单条目受 `max_archive_entry_mb` 流式限制。
- 封面：文件名匹配 `(^|/)cover[^/]*\.(jpe?g|png|gif|webp|bmp)$`（IGNORECASE）→ basename 含 "cover" → 任何图片第一个；无 → 文字封面。带 `w` 时解码缩到 ≤w 并编码 JPEG q84。
- image_names 排序：普通字典序 `sorted()`（**不是**自然排序）。

**FB2**（formats/fb2.py）：XML 全量读入（受 max_book_mb）；`description/title-info`：book-title/lang/author(first+middle+last+nickname 空格拼接，逗号连接)；章 = 每个 `body` 下直属 `section`（title → heading level=section 深度；subtitle → level+1；p、v/text-author/date/td/th → paragraph；empty-line → separator；image → `<binary id>` base64 图；嵌套 section 递归 level+1；epigraph/cite/poem/stanza/annotation/table/tr 同层递归）。无章 → 单匿名章。封面 `coverpage/image href`（去 `#`），退化第一个 binary。XML 解析必须禁实体/DTD（防 XXE）。

**漫画容器**（formats/comics.py）：
- 扩展名映射：`.cbz/.zip`→ZIP；`.cbr/.rar`→RAR（需系统 unrar/bsdtar，缺失 → 415 reason "需安装 unrar"）；`.cb7/.7z`→7z（需 py7zr，缺失 415）；`.cbt/.tar`→TAR；目录或单个图片文件 → 文件夹图集；`.cbz3` 在 COMIC_EXTS 中但无容器实现（按 ZIP 处理会失败——移植时注意）。
- 收录规则：图片扩展名白名单 `IMAGE_EXT = .jpg .jpeg .png .webp .gif .bmp .avif .jfif .tif .tiff`；`should_skip_entry`（隐藏段 `.` 开头、`./`、`..` 为路径语义不算隐藏；glob 匹配规范化路径与 basename，`*/X` 也匹配 basename）；`posixpath.normpath` 后 `is_unsafe_entry_name`（空、`.`、`..`、`../`开头、`/`开头、首段含 `:`）拒绝（zip-slip/盘符/ADS）。
- ZIP 条目名编码：flag_bits & 0x800 → 原名；否则按 cp437 还原原始字节，依次尝试 utf-8/gbk/big5/shift_jis 解码，取第一个"合理"（无 U+FFFD、控制字符仅 \t）结果。
- 规范化后同名条目：保留自然排序较小者并告警。目录条目、非图片跳过。图片总声明尺寸 > `max_archive_total_mb` → PARSE_FAILED。读取时流式强制 `max_archive_entry_mb`（7z 只能读后校验）。
- 页序：`natural_key` 自然排序（§4.9）。
- ComicAdapter：`page_count = len(names)`；页号 1 基；`meta` mode=fixed、`supports_json=false`。
- `page_bytes`：`fit=original` 且原图字节 ≤ `max_page_bytes` → 原字节直通（mime sniff）；超限强制缩放（target_w = max(viewport_w×scale clamp, 原图宽)，即保持原图分辨率优先，仅编码转 JPEG）。`fit=viewport`：target_w = round(viewport_w × clamp(0.5..2.0, scale))，解码（EXIF 校正 + 源像素上限）→ 只缩不放大 → 编码：原 mime 非 PNG 或无透明 → JPEG（quality 参数），否则 PNG。
- `pair_bytes`：第 page 与 page+1 页各缩放到 target_w 后横向拼接（白底、垂直居中），rtl 时反转顺序，JPEG 输出。末页无右页时返回单页。
- **条漫 strip**：`strip_pages(viewport_h, overlap)`：`vh=max(64,viewport_h)`，`overlap_eff = min(overlap, vh//2)`，`step = max(1, vh−overlap_eff)`；对每张图（probe 失败按单页 vh 处理）从 y=0 起切 `min(vh, h−y)` 高的片，y+=step。`StripPage{image_index, y0, height, is_first_of_image, is_last_of_image}`。`strip_slice_bytes`：整图解码（缩放 0 = 不缩）→ 裁 `[0, y0, w, y0+height]` → 不缩放直接编码（PNG/GIF 源 → PNG 无损，其余 JPEG quality）。锚点 `(0, image_index, y0)`。
- strip 触发条件：layout 请求 `mode="strip"`（客户端决定；`page_is_long_strip` = aspect ≥ strip_min_aspect 仅作提示）。

**PDF**（formats/pdf.py）：pypdfium2（PDFium）。fixed。`page_native_size` = 页面 pt × 2.0（`DEFAULT_RENDER_PT_TO_PX`，约 144dpi）。`fit=original`：scale = 2.0 × clamp(0.5..2.0, render_scale)；`fit=viewport`：scale = viewport_w × clamp(scale) / w_pt。`_clamp_scale`：w_pt×scale × h_pt×scale > `render.max_page_pixels` 时 scale ×= sqrt(max_pixels/pixels)。渲染 → PIL RGB → JPEG（quality 参数，prefer_jpeg 恒 true）。封面：第 1 页 viewport w=480 q82。pair：两页各按 viewport 宽渲染后 hstack；末页落单。metadata Title/Author。

**MOBI/AZW3**（formats/mobi.py）：探测 `ebook-convert`（PATH）；缺失 → 415 reason "需安装 Calibre"。转换到 `cache/converted/{book_id}.epub`（先写 `.tmp` 再原子 replace；每本书一把进程内锁；子进程超时 = parse_timeout_s × 4，最少 60s；exit≠0 或产物空 → PARSE_FAILED，stderr 尾 500 字节入 message）。此后完全走 EPUB 路径，meta/document 的 `format` 字段改回原格式（mobi/azw3）。

### 4.4 分页/布局引擎（layout/engine.py、services/layouts.py）

- **layout_id**：`layout_key = "L" + sha1( json.dumps({geometry: profile.geometry_dict(), parser: PARSER_VERSION("1"), engine: LAYOUT_ENGINE_VERSION("1"), font: fonts.resolve_id(family)}, ensure_ascii=False, sort_keys=True, separators=(",",":")) .encode("utf-8") ).hexdigest()[:12]`。注意：除几何外还纳入解析器版本、分页引擎版本与**已解析字体 id**（"auto" 会解析为实际字体）。（`LayoutProfile.layout_id()` 方法只哈希几何，服务层实际使用 `layout_key`。）
- **固定版式 PageMap**：`build_fixed_pagemap`：第 i 页（1 基）anchor=(0, i, 0)，end_anchor=(0, page_count, 0)，chapters=[]。`build_strip_pagemap`：第 i 片 anchor=(0, image_index, y0)，end_anchor=最后片的 (0, image_index, y0+height)。
- **flow 分页**（FlowLayoutEngine）：
  - 字号：正文 `font.size_px`；标题按 `_HEADING_RATIO = {1:1.8, 2:1.5, 3:1.3, 4:1.15, 5:1.05, 6:1.0}` 取整（≥8）；行高 = round(字号 × font.line_height, 2)；separator 高 = round(正文行高 × 1.6, 2)。
  - 折行：惰性贪心。token 化：空白串（空格/tab）一个 token；`is_wide_char`（码段表见 fonts.py：0x1100-0x115F、0x2E80-0x303E、0x3041-0x33FF、0x3400-0x4DBF、0x4E00-0x9FFF、0xA000-0xA4CF、0xAC00-0xD7A3、0xF900-0xFAFF、0xFE10-0xFE19、0xFE30-0xFE6F、0xFF00-0xFF60、0xFFE0-0xFFE6、0x1F300-0x1FAFF、0x20000-0x3FFFD）单字一个 token；其余连续非空白为一个 token，但按**绝对位置**每 1024 字符强制断开（`MAX_TOKEN_CHARS`，保证翻页续排与顺序排版 token 边界一致）。行宽超限（EPS=0.01）→ token 推回下一行；行首禁则字符集 `。，、！？：；）」』】》〉"'\u2026—·%!?,.:;)]}>` 且精确宽度下 `width+tw ≤ limit×1.08` → 允许 8% 溢出；单 token 超行宽 → 字符级硬断。首行缩进 = indent_chars × 字号像素（以 `\u3000` 前缀加入行文本；仅"从块开头排版"时给，offset>0 的续块不给）。超长块（>200000 字符，`LONG_BLOCK_CHARS`）分段折行。全空白段：首段产出一个缩进占位空行，后续段不产出。末尾纯空白不产出行。
  - 度量：宽字符恒 = 字号 + letter_spacing（**查表，不走字体**）；其余字符 = FreeType getlength + spacing（char/text 两级缓存）。长 token（>256 字符）只量到超过 limit 为止（返回"是否精确"标志，避免 O(n²)）。
  - 单页 `layout_page(start_anchor)`：从锚点惰性产条目（每章先产 ChapterMark——不占高度，chapter_new_page=true 且页非空且章号变化 → 在此断页）；条目高+space_after 累计 > avail_height+EPS → 换页；单条目超页高（防御）也换页。页 anchor = 首条目 anchor；章节归属 = **首条目**的章（ChapterMark 可能被并入上页）；first_line = 首条目为文本行时的文本。next_anchor = 断点条目 anchor。
  - `build()`：从 (0,0,0) 迭代 layout_page 直到 next_anchor 为空；防御：页数 > 500_000（MAX_PAGES）→ PARSE_FAILED；next_anchor 不前进 → 结束。章起始页 = 该章**任一条目**首次出现的页（chapter_new_page=false 时章可从页中间开始）。图片条目：显示尺寸 = 原始 w/h 缩入 `avail_w×image.max_width_ratio` 与 `avail_h×image.max_height_ratio`，且不超过整页可用高；space_after = paragraph.spacing_px；标题无 space_after。
- **锚点约定**：`{chapter, block, char}`；char 为块内纯文本偏移（缩进 `\u3000` 计入行文本但行锚点取块内偏移）。fixed 锚点 `(0, 页序号0基, 0)`；strip `(0, image_index, y0)`。

### 4.5 锚点 ↔ 页码（layout/anchors.py）

- 页 → 锚：`pagemap.record(page)`（1 基数组下标）；越界时取最后页（page>total）或第一页。
- 锚 → 页：锚点单调递增 → 二分查找"最后一个 anchor ≤ target"的页；target < 首页锚 → 第 1 页（target≠首页锚则 clamped）；target ≥ end_anchor → 最后一页 + clamped=true。
- `locate` 返回：page、total_pages、ratio=(page−0.5)/total clamp [0,1]、chapter_index/chapter_title（**定位页**的 record 章节，与 /pages/{n}/meta 一致）、anchor_chapter_index（target 章在 pagemap.chapters 中存在且未 clamp 时 = target.chapter，否则 = 页章节）、clamped。

### 4.6 ETag / 缓存键 / HTTP 缓存（context.py、routes_read.py、routes_books.py）

- **page ETag**：`'"' + ":".join(parts) + '"'`；parts = `[layout_id, theme或"-"(fixed时), scale_tag, str(page)]` + fixed 时追加 `[fit, str(quality or 0), "pair"或"-", "rtl"或"-"]`。`scale_tag`：round(scale,2)；若与 round(,1) 相同输出 1 位小数（如 `1.0`），否则 2 位。
- **page_cache_key**（内存 LRU 键，tuple）：`[book_id, layout_id, page, theme或""(fixed时), round(scale,2)]` + fixed 时 `[fit, int(quality or 0), int(pair), int(rtl)]`。
- **cover ETag**：`"{book_id}:w{w}"`；`Cache-Control: public, max-age=86400`。
- **asset ETag**：`"{book_id}:asset:{stable_hash(path)}"`（sha1 前 16 hex）；`Cache-Control: public, max-age=31536000, immutable`。
- **page**：`Cache-Control: private, max-age=31536000, immutable` + `Accept-Ranges: bytes`（仅 fixed/strip 且 original）。
- If-None-Match 比较：支持 `*`、`W/` 前缀剥离、逗号分隔多值；字符串精确比较（区分大小写）。
- Range 解析：仅 `bytes=` 前缀；只取第一个逗号段；`a-b` / `a-` / `-n`；语法错 → 忽略（200 整页）；`start ≥ total` 或 `end<start` → 416。

### 4.7 PNG/JPEG 渲染（render/raster.py、render/jsonout.py、util/imaging.py）

**flow 页**：
- 画布 = viewport × scale（scale clamp 0.5..2.0；`clamp_scale` 循环每次 −0.05 直到 `w×h ≤ render.max_page_pixels` 或 0.5）。RGB，背景 = 主题 bg。
- 主题表 `THEMES`（bg / fg）：white `#FFFFFF/#222222`；paper `#F5F0E6/#2B2B2B`；sepia `#F0E2C8/#3B2E1E`；green `#CCE8CF/#1F2A1F`；dark `#111418/#C8C8C8`。未知 theme → white。`muted = blend(fg,bg,0.45)`（线性插值）。
- 绘制：背景 → 文本行 → 图片；坐标 = margin×scale + item.y×scale；文本基线垂直居中于行框（`(box_h − (ascent+descent))/2`，负则 0）；letter_spacing 绝对值 >0.01 时逐字绘制（间距 = getlength + spacing）；bold = 偏移 `max(1, 字号/32)` 二次绘制（不改行宽）；italic 无渲染处理（仅标记）。separator：在条目高度中点画水平线，x 从 8% 到 92% 可用宽，muted 色，线宽 max(1, round(scale))。图片：解码（EXIF 校正、max_source_pixels）→ resize 到 display×scale（LANCZOS）→ 水平居中粘贴（RGBA 用 alpha 合成）；加载失败画灰框 + 文字"图片无法显示"。页码/章节名**不画**（margin.bottom 预留给客户端）。
- 编码：`is_photo_page`（图片面积 ≥ 内容区×0.5，或（无文本行且最大图宽 ≥ 可用宽×0.8））且 `jpeg_for_photo_pages` → JPEG q85；否则 PNG（optimize）。带透明且非照片页 → PNG。默认（封面等）`encode_image(prefer_jpeg=True)`：无透明→JPEG，有透明且 force_png→PNG，prefer_jpeg=False→PNG。
- 封面文字图（text_cover）：宽 clamp 80..1024，高 = w×1.42；配色 = `COVER_PALETTE[int(sha1(title)[:8],16) % 6]`（6 组 (bg,fg)），顶部装饰条；标题字号 w×0.13，逐字符贪心换行最多 4 行（超出加 "…"）；作者字号 w×0.082 底部最多 2 行；JPEG q88。

**JSON 模式页（build_page_json）**：服务端决定分页边界，客户端逐行绘制不重排。结构：
```json
{"page","total_pages","anchor":{chapter,block,char},"chapter_index","chapter_title","mode",
 "blocks":[{"type":"heading","level","lines":[str],"font_size","y","height","bold"?,"italic"?}
          |{"type":"paragraph","lines","font_size","y","height","bold"?,"italic"?}
          |{"type":"image","asset":"<完整 /api/v1/books/{id}/asset?path=<urlencoded>>","display_w","display_h","y","height"}
          |{"type":"separator","y","height"}],
 "font":{family,size_px,line_height,letter_spacing_px},
 "margin":{left,right,top,bottom},
 "viewport":{w,h},
 "paragraph":{"spacing_px","first_line_indent_chars":0,"indent_applied_in_lines":true}}
```
相邻同类文本条目（kind/chapter/block/level/font_size 相同）合并为一个 block，`height` 累加。注意 `first_line_indent_chars` 恒下发 0（缩进已含在 lines 的 `\u3000` 前缀里）。

**imaging 细节**：`sniff_mime` 魔数表（JPEG FFD8FF、PNG 89 50 4E 47 0D 0A 1A 0A、GIF87a/89a、RIFF..WEBP、BM、II*\0 / MM\0*）失败退扩展名表；`probe_size` 读头 + EXIF Orientation 5/6/7/8 宽高互换；`decode_for_display`：JPEG 可 draft 降采样、`ImageOps.exif_transpose`、像素上限超限 → ImageTooLargeError。缩放一律 LANCZOS。

### 4.8 gallery（文件夹图集）与书库索引（services/gallery.py、services/library.py）

- **gallery book_id**：`sha1(utf8("gallery:" + normalize_vpath(vpath, allow_empty=True))).hexdigest()[:16]`。目录内容增删不变；目录改名/移动失效。
- 注册表 `cache/galleries.json`：`{book_id: {path, recursive, title, page_count, created_at, updated_at}}`（ensure_ascii=false，indent=1，tmp+os.replace 原子写）。
- 注册：vpath 必须 normalize 合法且 resolve 存在；单图片文件 → 单页图集（recursive 强制 false）；目录 → list_images（跳过规则同容器、自然排序、符号链接不计）；图片数 < max(1, min_images_in_folder) → 415。
- **书库索引 `cache/index.json`**：`{"version":1,"scanned_at":float,"books":{vpath:{book_id,format,size,mtime}},"gallery_counts":{vpath:{count,mtime}}}`（紧凑分隔符）。增量判据：size 相同且 |mtime 差|<1e-6 且 format 相同 → 复用 book_id。扫描时隐藏文件/目录跳过、符号链接跳过、目录排序 natural；图片目录计数（gallery_counts），`gallery.recursive=true` 时自底向上 rollup 且禁用计数缓存。
- 虚拟路径规则（util/paths.py）：unquote → strip → `\`→`/` → 补前导 `/` → 段过滤（空、`.`；`..` 报错；含 `:` 报错（盘符/ADS）；段首 `.` 之前的 Windows 保留名 con/prn/aux/nul/com1-9/lpt1-9 报错）→ 段内空白**不裁剪**。真实路径解析：root.resolve() 与候选 resolve() 必须是根内（不跟随软链逃逸）。

### 4.9 自然排序（util/natural.py）

`natural_key(name)`：按正则 `(\d+)` split 交替成 文本/数字段；文本段 → `(0,0,part.casefold())`；数字段 → `(1,int(part),"")`；tuple 逐元素比较。目录列表、容器页序、扫描顺序均用它（忽略大小写、数值比较：`2.jpg < 10.jpg`）。

### 4.10 /progress、/search 细节

- **progress**（services/progress.py）：文件 `cache/progress.json`，JSON 对象 `{"{book_id}:{device}": record}`；record 含外发的 6 字段 + 内部 `updated_at_ts`（float epoch）。device 规范化：strip → 空 → `"default"` → 剔除不可打印与 `:"\/` → 截断 64 字符。上限 5000 条（最旧 updated_at_ts 淘汰）。整文件重写、≥1s 节流、关停 flush。GET 未指定 device 返回该书最新一条。
- **search**（services/search.py）：在 DocumentModel 上逐章逐文本块子串查找；**纯 ASCII 查询词忽略大小写**（lower 后长度不变才可用，否则退回敏感）；扫描上限 500 命中（total 继续计数到 500 即止，`limited = truncated or total > len(results)`）；snippet = 命中前后各 30 字符，越界侧加 `…`；带 layout_id（flow）时逐条用 page_of_anchor 换算页码。

---

## 5. 认证

来源：`server/app/api/deps.py`。

- Header 名：`X-Auth-Token`。
- 配置 `server.auth_token` strip 后为空 → 完全跳过校验（无鉴权模式）。
- 非空时：请求头值 strip、UTF-8 编码后与期望值做**常量时间比较**（`secrets.compare_digest`，bytes 级——非 ASCII 用 str 比较会抛异常，必须编码成 bytes）。失败 → 401 `UNAUTHORIZED`，message "令牌缺失或错误"，detail `{"auth_required": true}`，响应头 `WWW-Authenticate: X-Auth-Token`。
- 豁免端点：仅 `GET /api/v1/server/info`。`/`、`/healthz`、其余全部端点均需校验。开启鉴权后交互式文档（/docs /redoc /openapi.json）关闭。

C++ 注意：必须使用常量时间比较（如逐字节 XOR 累积），且先 strip 两端空白。

---

## 6. 缓存目录结构与共用性

来源：`config.py` 派生路径、`cache/parsed.py`、`cache/store.py`、`services/*.py`、`formats/mobi.py`。根：`{base_dir}/cache/`。

```
cache/
├── index.json                       # 书库索引（version/scanned_at/books/gallery_counts）——JSON 结构语言无关，可共用
├── galleries.json                   # 图集注册表——可共用
├── progress.json                    # 进度 KV——可共用
├── parsed/
│   └── {book_id}.{PARSER_VERSION}.json.gz    # gzip(UTF-8 JSON, DocumentModel.to_dict(), 紧凑分隔符, ensure_ascii=false)
├── layouts/
│   └── {book_id}/{layout_id}.json.gz         # gzip(JSON)：{"book_id","layout_id","profile":LayoutProfile,"pagemap":PageMap.to_dict()}
└── converted/
    └── {book_id}.epub               # MOBI→EPUB 转换产物——可共用
```

**可共用（C++ 直接读写）的条件与风险**：
- `parsed/*.json.gz`、`layouts/*.json.gz`：格式是纯 JSON（gzip 压缩），**结构语言无关**；只要 C++ 端实现 `DocumentModel`/`PageMap`/`LayoutProfile` 的 `to_dict/from_dict` 语义完全一致（字段名、缺省值、end_anchor 缺失推导、PARSER_VERSION/ENGINE_VERSION 相同），即可互相读取。写入需原子（`*.tmp` + rename）。损坏文件（解压失败/JSON 错/model 校验失败/book_id 或 layout_id 不符）直接删除重建。
- `index.json` / `galleries.json` / `progress.json` / `converted/`：纯数据，可直接共用。
- **不可共用/需谨慎**：
  - 页面图片内存 LRU（进程内，无落盘）——各自实现；
  - ETag/缓存键不落盘，只需算法一致；
  - gzip 内 JSON 的键序、float 精度（`round(x,2/3/4)`）与 `ensure_ascii=false` 的 UTF-8 中文必须一致，否则跨实现读取虽不报错但会产生语义偏差；
  - `PARSER_VERSION`/`LAYOUT_ENGINE_VERSION`：若 C++ 分页/解析结果与 Python 有任何差异（见 §8），**必须递增版本号**避免读到对方的错误缓存——但这会使切换服务器时缓存全量重建，属可接受代价；若确认逐字节一致则共用。

---

## 7. C++ 技术选型建议（MSVC C++20）

原则：优先 header-only 或可 vendored 单库；避免重运行时依赖。

| 领域 | 建议 | 理由 / 备选 |
|---|---|---|
| HTTP 框架 | **cpp-httplib**（header-only，带 Range/分块/SSL 可选） | 单文件、线程模型简单（`svr.new_task_queue = thread pool`），与"每请求在线程池跑 CPU 活"的 Python 模型对应。备选：Drogon（性能好但重）、libuv+自写（不建议）。并发去重/超时/KeyedLocks 用 `std::mutex`+`std::condition_variable` 自实现（§concurrency.py 语义），超时用 detached `std::jthread` + future |
| YAML | **yaml-cpp**（可 vendored） | 事实标准；config 结构简单，加载后映射到与 §2 完全一致的默认值+校验（重名书库、非法段名等启动即报错） |
| JSON | **nlohmann/json**（header-only） | 与 Python dict 语义接近；`ensure_ascii=false` 对应直接输出 UTF-8；注意 Python `json.dumps(sort_keys=True)` —— 参与 layout_id 的 JSON 必须按键排序序列化，且 float 输出格式需与 Python 一致（见 §8 风险） |
| gzip | **zlib**（或 miniz header-only） | 只需 gzip 流包装（`gz` 格式，非 raw deflate） |
| zip | **miniz**（header-only，读为主）或 kuba--/zip | 需要按条目随机读取、data descriptor/flag 0x800 判断、cp437 还原原始字节（miniz 暴露原始 filename bytes）；EPUB 也用它 |
| tar | 自写 ustar/pax 读取（几百行）或 libtar | CBT 低优先级 |
| 7z | **可选**：链接 7z.exe/7za.exe 子进程（对齐 Python 的"外部后端"语义）或 ppmd/lzms 难以 vendored | Python 用 py7zr；C++ 用 `lib7zip`+7z SDK 复杂。最务实：缺失时报 415 reason "缺少 7z 后端"，行为等价于 Python 缺 py7zr |
| rar | **可选**：调用系统 `unrar`/`bsdtar` 子进程 | 与 Python 完全同构（rarfile 本来就是外部工具壳）；缺失报 415 "需安装 unrar" |
| 图片解码/编码 | **stb_image**（解码，header-only）+ **libjpeg-turbo**（JPEG 编码带 quality）+ **fpng** 或 stb_image_write（PNG） | stb_image 不支持 EXIF 方向与渐进 JPEG 优点欠缺，可接受；JPEG 编码质量参数（1..100、progressive、optimize）建议 libjpeg-turbo 对齐 Pillow。像素上限在解码前用 probe_size 拦截。备选集成方案：**独立 vendored Skia/FreeImage 过重，不建议** |
| 字体光栅化 | **FreeType**（vendored，含 OTF/TTF/TTC；中文字形无问题） | 度量 `getlength`、ascent/descent、逐字光栅化（渲染文本可用 stb_truetype 但 FreeType 与 Pillow 行为最接近）；CJK 宽度走码段查表（§4.4）不依赖字体，天然一致。"能画中文"探测：光栅化 `中` 与 `\uE123`（.notdef 基准）比较像素。粗体/斜体模拟与 Python 一致（二次描边/仅标记） |
| PDF（可选，可降级） | **首选 pdfium**（Google pdfium binaries，与 Python pypdfium2 同引擎——渲染结果最接近）；**降级方案**：不内置 PDF 支持，`/files` 列表对 pdf 置灰（openable=false，reason "缺少 PDF 后端"），API 返回 415 | pdfium 是 C API、官方有预编译 MSVC 二进制；MuPDF 许可（AGPL）需注意 |
| 编码检测 | BOM + 自写 UTF-16 启发 + **GB18030 兜底**（Windows 直接 `MultiByteToWideChar(54936)`，跨平台用 ICU 或 vendored `-libiconv`）；charset-normalizer 的 chaos 打平韩/中文修正可用简化启发（NUL/有效性统计）替代 | 检测链在 §4.2；charset-normalizer 无 C++ 等价物，允许在此处行为有差异（见 §8） |
| SHA-1 | 公共域 **sha1.c**（或 Windows BCrypt） | book_id / ETag / layout_id / gallery_id 全依赖，输出小写 hex |

工程结构建议对齐 Python 分层：`http/`（路由+鉴权+错误模型）、`config/`、`formats/`（txt/epub/fb2/comic/pdf/mobi + registry）、`layout/`（engine/fonts/anchors）、`render/`（raster/jsonout）、`cache/`（lru/parsed/store）、`services/`（library/gallery/layouts/progress/search）、`util/`（paths/natural/textutil/imaging/xml/concurrency）。

XML（EPUB OPF/NCX、FB2）：**pugixml**（小型、可禁 DTD/实体，满足 XXE 防护）。HTML 解析（EPUB 正文）：**lexbor** 或 **myhtml**（对齐 BeautifulSoup 的容错解析 + 块级标签遍历语义）。

---

## 8. 移植风险与可接受的行为差异

### 必须逐位一致（否则破坏"二选一可替换"）

1. **哈希四件套**：book_id（sha1(size ascii + 1MiB 头) 前 16 hex）、gallery_id（sha1("gallery:"+vpath) 前 16）、stable_hash（sha1 前 16，ETag/封面配色用）、layout_id（sha1(排序 JSON) 前 12 + "L" 前缀）。任何偏差 → 客户端进度/缓存全失效。
2. **ETag 与 page_cache_key 公式**（§4.6），含 scale 的 1/2 位小数规则与 fixed 模式的占位符（`"-"`/`""`）。
3. **缓存落盘 JSON 的结构**（§6），含 `PARSER_VERSION`、`LAYOUT_ENGINE_VERSION`、`INDEX_VERSION`、galleries/progress 的键与字段名。
4. **错误码枚举、HTTP 状态映射、默认 message 文案、detail 键**（§1.2）——Android 客户端按 code 分支。
5. **LayoutProfile 校验边界与默认值**（§3.1）——pydantic 会拒绝越界请求体，C++ 必须同样返回 400 BAD_REQUEST（detail.errors 结构）。
6. **自然排序、虚拟路径规范化与安全规则**（§4.8/4.9）——影响页序与路径兼容。
7. **Range/304/Cache-Control 语义**（§1.3、§4.6）。

### 高风险点（Python 行为的隐蔽细节）

- `is_wide_char` 码段表必须逐段照抄（含 emoji 0x1F300-0x1FAFF 与扩展 B 区），否则折行位置与 Python 不同页——虽然各自内部自洽，但同书跨服务器切换会跳页。
- token 按**绝对位置**每 1024 字符断开的规则、8% 避头尾溢出、首行缩进只在块首给——都直接影响页码。
- 图片 `probe_size` 与解码都必须做 EXIF 方向 5/6/7/8 宽高互换，否则条漫切片与 meta.aspect 错位。
- TXT 编码检测：BOM 表顺序、无 BOM UTF-16 的 0x00 占比 ≥0.2 + 可读性复核、GB18030 最终兜底——至少这些可精确复刻；charset-normalizer 部分允许简化。
- JSON 模式 `first_line_indent_chars` 恒 0 + `indent_applied_in_lines:true`；`json` 字段名 alias。
- ZIP 条目名 cp437→(utf-8/gbk/big5/shift_jis) 启发、`./` 前缀与盘符拒绝、`__MACOSX/` 等 skip glob 的三重匹配（canonical、basename、`*/X`→basename）。
- PDF 页号 1 基、pt×2 基准倍率、`max_page_pixels` 的 sqrt 降倍。
- 进度 `updated_at` 是 UTC `YYYY-MM-DDTHH:MM:SSZ`（秒级），内部另存 `updated_at_ts` 浮点。

### 可接受的行为差异（建议记录在 README）

1. **PNG/JPEG 编码字节级差异**：编码器不同（libjpeg-turbo vs Pillow、zlib 参数 vs PIL optimize）→ 同参数输出字节不同 → ETag 不含内容哈希所以无碍；体积/画质差异可接受。
2. **字体度量微差**：FreeType 版本与 Pillow 内嵌 FreeType 的 hinting/字距可能差亚像素级 → 折行极端情况下某行多/少一个字 → 页码可能偏移。**缓解**：递增 `LAYOUT_ENGINE_VERSION` 会同时影响两种实现；更务实的是接受"切换服务器后首次 /layout 需重排"（同 layout_id 下 PageMap 缓存以先写者为准，客户端无感，只是页码可能±1 页）。若要严格一致，需用同一 FreeType 版本并禁用 hinting（Pillow 默认 `layout_engine=basic`/raqm 视构建而定，需实测对齐）。
3. **charset-normalizer 的复杂统计模型**：允许以简化启发替代；差异表现为极少数无 BOM 短样本 TXT 编码判错——可用 `encoding_overrides` 补救。
4. **EPUB 主路径**：Python 优先 ebooklib（结果与手写 OPF 路径"保证一致"）；C++ 只实现 zip+OPF 一条路径即可（Python 的兜底路径语义相同）。
5. **后台扫描/落盘节拍的毫秒级差异**、`maybe_save` 触发时机——外部不可观测。
6. **日志格式与启动横幅**文案——非契约。
7. **`--no-scan` 语义**：Python 现实现会同时关闭后台扫描（可能是 bug）；C++ 可实现为"不阻塞启动、后台补扫"（更符合帮助文本），在文档中标注即可。
8. **`.cbz3` 扩展名**：Python 声明支持但无对应容器；C++ 可按 ZIP 处理（更好的行为）或同样报错。
9. **超时行为的实现差异**：Python 用守护线程（超时后任务仍在跑，靠 in-flight 去重挡重复）；C++ 可用可取消任务真正中断，对外契约（PARSE_TIMEOUT→422、busy→503 Retry-After:5）保持一致即可。
10. **文字封面像素**：字体不同导致封面观感差异——配色（sha1 决定）与布局参数一致即可。

### 测试对齐建议

Python 侧已有完整测试（`server/tests/`：encoding、anchors、cache invalidation、containers、epub_fb2、api、paths_security、fonts、concurrency、adversarial_fixes）。移植时逐个用相同输入数据对拍：尤其 `test_encoding.py` 的样本集、`test_anchors.py` 的页码断言、以及用 Python 服务器生成的 `cache/parsed`、`cache/layouts` 文件让 C++ 端读取并渲染同一页比对。
