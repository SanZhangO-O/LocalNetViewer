# LocalNetViewer Server (C++)

Python 服务器（`../server`）的 C++ 移植版：**二选一运行**。两者共用：

- 同一份 `server/config.yaml`（书库、端口 8765、令牌等；`--host/--port` 可覆盖）
- 同一书库目录
- 同一缓存目录 `server/cache/`（`index.json` / `galleries.json` / `progress.json` /
  `parsed/*.json.gz` / `layouts/*.json.gz` 结构语言无关，可互相读取）

HTTP API 契约与 Python 版一致（`docs/PORT_SPEC.md` 为移植规格）。

## 构建（Windows / MSVC）

```bat
powershell -ExecutionPolicy Bypass -File server-cpp\build.ps1
```

- 依赖已 vendored 在 `thirdparty/`（cpp-httplib、nlohmann/json、miniz、stb、yaml-cpp、
  pugixml、FreeType），**随仓库提交**，无需联网、无需额外安装；产物：`build\lnvserver.exe`。
- 需要 MSVC（VS 2022/2026 或 Insiders，C++20）+ CMake ≥ 3.24 + Ninja。
- `thirdparty/` 是**裁剪过**的：只保留编译与 CMake configure 需要的文件（13 MB / 892 文件，
  原始上游 51 MB）。各依赖的版本与来源见 `thirdparty/README.md`；升级依赖时按该文件的
  步骤重新 vendored，并**重新应用** `patches/cpp-httplib-lnv-range.patch`（build.ps1 会校验
  补丁标记，缺失时直接报错而不是静默产出行为不一致的二进制）。
- `build.ps1 -Debug` 构建调试版；`-Run` 构建后直接以 `server/config.yaml` 启动。
- **构建被中断过就加 `-Clean`**：MSVC 预编译头（`build/…/cmake_pch.cxx.pch`，本项目约
  350 MB）在中断的构建里可能只更新一部分目标文件，导致同一进程内不同 TU 的结构体布局
  不一致（ODR 违规），表现为**随机堆损坏**（0xc0000374 / 无效 free），而不是编译错误。
  实测同一份源码：被打断过的 `build/` 在并发压测下必崩，`-Clean` 重建后 120 轮无问题。

## 运行

```bat
server-cpp\build\lnvserver.exe --config server\config.yaml
server-cpp\build\lnvserver.exe --config server\config.yaml --port 8799   # 覆盖端口
```

CLI 与 Python 版 `run.py` 一致：`--config` / `--host` / `--port` / `--no-scan` / `--log-level`
（未知选项、非法端口、非法 log-level 都会报错并以退出码 2 结束，与 argparse 相同）。

## 测试

```bat
cd server
.venv\Scripts\python -m pytest ..\server-cpp\tests -q
```

`server-cpp/tests/` 是回归测试（真实启动 `lnvserver.exe` + 临时书库/缓存）：容器条目映射、
编码探测边界、深层嵌套、压缩包守卫、图片解码、HTTP 契约（鉴权/错误信封/整数边界/Range）、
解析闸门并发语义、缓存落盘与关停。**修改移植代码后请先跑它**。

## 功能支持

| 格式 | 状态 |
|---|---|
| TXT | ✅（编码检测链 + 章节切分） |
| EPUB | ✅（zip + OPF/spine 手写解析，即 Python 的兜底路径） |
| FB2 | ✅ |
| CBZ / ZIP / CB7 / CBR / CBT / 图片文件夹 | ✅（CBR 需系统 unrar/bsdtar，CB7 需 7z.exe） |
| PDF | ⚠️ 需将 `pdfium.dll` 放到 exe 目录（可选后端，缺失时报 415） |
| MOBI/AZW3 | ⚠️ 需安装 Calibre（`ebook-convert`），转换缓存共用 `cache/converted/` |
| 图片 | JPEG/PNG/GIF/BMP 走内置 stb；**WebP/TIFF/AVIF 走系统 WIC 解码器**（Windows 自带 TIFF；WebP/AVIF 需系统装了对应扩展，缺失时该页报 422） |

## 与 Python 版的已知行为差异（可接受，详见 docs/PORT_SPEC.md §8）

- PNG/JPEG 编码器不同 → 图片字节不同（ETag 不含内容哈希，客户端无感）。
  PNG 走自适应行滤波 + miniz level 9，体积与 Pillow 相当；JPEG 走 stb（4:4:4、无优化/
  渐进霍夫曼），同质量下比 Pillow 大 20%~60%，q100 可到 2.5×。图像缩放用
  stb_image_resize2 的 Mitchell（stb 无 Lanczos 核），几何尺寸一致。
- 字体度量微差 → 极端情况下某书页码可能偏移 ±1 页（同 layout_id 下 PageMap
  以先写者为准）。行宽判定逐字累加，与 Pillow 的 `getlength` 同模型（实测
  两者对同一字符串结果一致），差异来自 FreeType 的 hinting/取整路径。
- TXT 无 BOM 短样本的编码判定为简化启发（charset-normalizer 的统计模型无 C++
  等价物）；个别文件可用 `encoding_overrides` 修正。**编码标签与 Python 对齐**
  （`utf_8`/`ascii`/`gb18030`/…），`meta.encoding` 逐字节相同。
- `fit=original` 的 Range 分块由 cpp-httplib 与处理层各处理一次的问题已修复
  （处理层清空 `req.ranges`），并在 vendored httplib 上打了标记为 `[LNV PATCH]`
  的补丁：语法非法的 Range 头按 RFC 9110 忽略（Python 同样忽略），不再提前返回 416。
  **补丁文件在 `patches/`，重新 vendor 时必须重新应用**（build.ps1 会校验）。
- 列表条目 `openable`/`reason` 会探测可选后端（unrar/bsdtar、7z、Calibre、
  pdfium.dll），与 Python 一致；探测结果进程内缓存。
- 解析并发：与 Python 同语义的解析闸门（同任务去重、并发上限 8、槽位先等 5s 再
  503、等待超过 `limits.parse_timeout_s` 报 422 `PARSE_FAILED{reason:timeout}`）。
  与 Python 一样，超时不会强杀任务：后台继续跑完、槽位随之释放；超时后的重试会
  命中同任务去重（同 Python）。`/document`（页/搜索入口）同样按书去重。
- 书库扫描与递归计数会跳过符号链接与 NTFS junction（按 reparse tag 识别，深度
  上限 256），避免 junction 成环导致无限递归/栈溢出；CPython `os.walk` 实际会
  跟进 junction（bpo-23407 未修），因此扫描结果可能有差异（仅能经 junction
  到达的文件不会进索引，但仍可在列表中浏览打开）。
- 外部解压后端（unrar/bsdtar/7z）按需把**单个条目**解到内存并全程限长（不再整包
  落盘）；列表/单条目读取有 120s 超时并在超时后杀掉整棵进程树（Python 无超时）。
  RAR 的“每条目一次 unrar 进程”与 Python rarfile 的逐文件进程语义一致；py7zr 是
  进程内读取，CB7 的逐条目 7z 进程相对更慢。适配器实例内对已读条目做 32MB 上限
  的缓存，同一请求内重复读取（probe 后再取页/切片）不会重复解压。
- 缓存文件与 Python 互相可读（parsed / layouts / index / galleries / progress），
  但 JSON 键序不同：Python 用插入序，本实现用 nlohmann 的按键排序；
  `PageRecord.y0/height` 在 Python 里是 int（`0`）、这里是 double（`0.0`）。
  EPUB 解析缓存的 `meta.parser` 固定为 `"zip+opf"`（Python 走 ebooklib 时为
  `ebooklib/(x, y, z)`）。语义一致，字节不逐位相同。
- **畸形压缩包**：中心目录声明的解压后大小与实际数据不符时，Python 可能正常读出
  （声明偏大）或直接 500 `BadZipFile`（声明偏小）；本实现统一 422 `PARSE_FAILED`
  （流式读取在上限处中止，不会按声明尺寸预分配内存）。
- **超过 `max_archive_entry_mb` 的单章**：本实现的 `load_model()` 会读入 spine 里所有
  章节，因此 `/meta` 就会 422；Python 的 `/meta` 只读元数据，仍返回 200（`/layout`
  才会失败）。默认上限 512 MB，只有极端单章文件才会碰到。
- **HTML 嵌套深度上限 200 层**：超过部分整棵子树被丢弃（Python 用 bs4/lxml 无上限）。
  实测 6 万层嵌套时 Python 得 0 字符/0 页，本实现同样 0 字符/0 页；正常文档
  （深度远小于 200）不受影响。若无此上限，6 万层嵌套会让进程栈溢出直接死掉。
- 未实现 FastAPI 的 `/docs`、`/redoc`、`/openapi.json`（返回 404）。
- `GET /render/fonts/{id}/file` 已带 ETag / Last-Modified / Accept-Ranges，并支持
  单段 Range（206；越界 416 带 `Content-Range: bytes */size`）。两处有意超集：
  本实现对 `If-None-Match` 返回 304，而 Python 的 Starlette FileResponse（0.50）
  只处理 Range/If-Range、不处理 If-None-Match（条件请求仍 200 全量重发）；
  多段 Range 本实现按第一段处理，Python 返回 multipart/byteranges。
- 进度 `anchor`/`page` 接受任意 JSON 整数（含 2^63..2^64-1）并原样落盘/回显，
  与 Python 的无界 int 一致；超出 2^64 的整数 nlohmann 解析为 double，会有精度差。
- 加密 EPUB：本实现读条目时检测加密并返回 422（提示需要密码）；Python 走 zipfile
  抛 RuntimeError → 500。
- 几何参数（`viewport`/`margin`）超过 32 位整数范围时本实现饱和到 `INT_MAX`
  （Python 无界 int 会继续参与布局计算）；正常取值远小于该范围。
- 少数编码标签差异：SJIS/EUC-JP 系文本 Python 可能因 charset-normalizer 的
  fingerprint 合并报 `cp932`/`euc_jis_2004`，本实现报 `shift_jis`/`euc_jp`
  （正文一致，`meta.encoding` 可能不同）。
- httplib 自身的错误（超长 URI → 414、超长请求头 → 400）现在也带统一 JSON 错误信封；
  Python 对超长 URI 返回 400。两者状态码不同但都返回可解析的错误体。
- **非 ASCII 令牌**：本实现按原始字节比较，可正常工作；Python 用 latin-1 解码后的头再
  比较，非 ASCII 令牌实际上永远匹配不上。建议令牌只用 ASCII 可见字符。
