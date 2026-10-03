# LocalNetViewer 全面评审报告（设计 / 界面 / 交互 / 实现）

评审范围：`server/app` 全部模块、`android/app` 全部 Kotlin 与清单/构建文件、`.e2e/shots` 端到端截图走查。
总体评价：工程质量高于平均水准——路径安全防护成体系、缓存失效有内容指纹、协程/防抖注释详实、错误映射统一。
但仍有若干**高优先级缺陷**集中在：进度落盘竞态、双页跨页页码换算失效、默认无鉴权 + 0.0.0.0、lxml XXE、底部工具栏不避让导航栏。

---

## 一、端到端截图走查发现（界面 / 交互）

### UI-1【高】正文最后一行被底部工具栏遮住 / 裁切
- 证据：`.e2e/shots/06_reader_page1.png` —— 第 1/32 页，正文最后一行"负责分页与渲染，客户端只按页取图；改字号或…"被截断在屏幕底缘，恰好被底部半透明工具栏盖住。
- 原因：服务端默认 `margin.bottom`（8dp，见设置面板"上下边距"）预留的空间**小于**客户端底部工具栏实际高度；客户端 overlay 直接画在页面图片之上。
- 建议：客户端把 overlay 实测高度（含导航栏 inset）随 viewport 上报，或服务端按 `overlay_bottom` 强制下限预留；至少把默认上下边距下限提高到工具栏高度。

### UI-2【中】漫画阅读页图片不居中、留白颜色不一致
- 证据：`.e2e/shots/13_comic_reader.png` —— 页面右侧有一条白色空隙，图片偏左；页面上下区域是灰白色、图片 Letterbox 又是白色，视觉割裂。
- 建议：fit=viewport 时让图片水平居中，页面背景色与主题背景一致。

### UI-3【中】设置面板暴露开发者单位
- 证据：`.e2e/shots/11_settings_sheet.png` —— 滑杆数值显示"18 sp / 1.60× / 16 dp / 8 dp / 6 sp"，dp/sp 对普通用户无意义。
- 建议：字号直接用"号"或相对倍数，边距用"小/中/大"或相对档位；同时滑杆当前值应可点按数字直接输入。

### UI-4【中】弹层内容可能超出屏幕（横屏/小屏）
- TocSheet 560dp、SettingsSheet 560dp、SearchSheet 420dp 硬编码高度上限（TocSheet.kt:79、SettingsSheet.kt:57、SearchSheet.kt:98）。横屏可用高度不足时内容顶满甚至裁剪（截图中"字体"一行已被底缘裁掉）。
- 建议：改为 `screenHeightDp` 比例（如 0.75f）。

### UI-5【低】浏览页分隔线过重 + 面包屑冗余
- 证据：`.e2e/shots/04_library_root.png` —— 行与行之间是粗灰条（像未优化的 divider），标题下"书库 / 测试库"面包屑与标题信息重复。
- 建议：用 M3 默认细 divider；根目录时不显示面包屑。

### UI-6【低】连接页输入框无外置 label
- 证据：`.e2e/shots/01_connect_screen.png` —— 两个 OutlinedTextField 只靠 placeholder 当标签，输入后标签消失；且无"粘贴示例格式 IP:8765"校验提示、无扫码入口（M4 规划项）。
- 建议：使用 Material 外置 label；地址输入错误时即时提示格式。

### UI-7【低】Slider 视觉可供性差、无无障碍语义
- 底部进度条与设置面板滑杆使用细竖线 thumb，难以辨识与拖拽；TalkBack 读不出页码进度（ReaderOverlay.kt:119-126）。建议恢复常规 thumb 或加 semantics。

---

## 二、Android 客户端（按严重程度）

### 高
| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| H1 | ReaderViewModel.kt:647-667 | `onCleared` 里另起不可取消的 `CoroutineScope(SupervisorJob()+IO)` 兜底落盘进度，与重新打开同一本书的新写入存在竞态：旧实例的写盘可能晚于新实例读取，用旧页码覆盖新会话 | 兜底落盘改用进程级 applicationScope 并按 bookId 串行化（Mutex/Channel） |
| H2 | ReaderScreen.kt:264-268 | 翻页回写 effect key 缺 `spreadStep`：切换"双页跨页"后 `pageOfIndex` 按旧步长换算，进度/页码/预取全部错位 | key 补 `state.spreadStep`，或 collect 内读 `vm.state.value` |
| H3 | ComposeUtils.kt:85-120 | `rememberStableInsets` 只增不减，旋转后残留另一方向的 inset（竖屏 bottom 残留到横屏），服务端按错误 inset 排版且永不恢复 | holder 以 orientation/viewport 尺寸为 key 重建 |
| H4 | ReaderOverlay.kt:104-128 | 底部工具栏未 `navigationBarsPadding()`，手势条会盖住进度 Slider；SnackbarHost、顶部 LinearProgressIndicator 同样不避让系统栏 | 补 insets 避让 |
| ~~H5~~ | ~~SettingsStore.kt + AndroidManifest.xml:10~~ | **【复核后撤回】** backup_rules.xml 与 data_extraction_rules.xml 均为 include-only（只含 lnv_progress），令牌所在 `lnv_settings` 已被排除，两文件注释也写明了这一设计 | 无需修改 |
| H6 | PageImage.kt:114-151 | 1x 状态捏合无法放大（transformable 仅放大后才挂载）；双击放大中心固定为画面中心而非点击点 | 用可区分双指/单指的手势层；双击按点击点计算 offset |

### 中
- **M1** ReaderScreen.kt:334-354 条漫模式裸用 AsyncImage：无加载指示、无失败重试占位（PagedReader 有）；也不支持缩放。
- **M2** BrowserScreen.kt:210-247 面包屑深层路径溢出不可见，且触摸目标仅 ~20dp（<48dp 无障碍标准）。
- **M3** 见 UI-4 弹层高度硬编码。
- **M4** BrowserScreen.kt:176 `items(key={it.path})` 重复 path 直接崩溃（SearchSheet 已防，浏览页没防）。
- **M5** PageImage.kt:74-81 重试在 URL 加 `_r=N` 绕过 Coil 缓存并与预取 key 失配，多次重试在磁盘缓存各存一份。
- **M6** ReaderViewModel.kt:450-476 预取 `runCatching` 吞掉 CancellationException（与项目自身 `runCatchingIo` 原则相悖）。
- **M7** ApiCall.kt:67 HttpException 的 errorBody 读取在主线程做网络 IO，有 ANR 风险。
- **M8**【复核修正】ratio 公式确实不一致，但位置更正为：ReaderViewModel.kt:266、394 用 `page/total`，651、678 用 `(page-0.5f)/total`（跳页换算与进度保存两套口径），同一页两条路径写出的 ratio 不同，恢复位置会漂移约一页。
- **M9** ReaderScreen.kt:252-262 跳页 effect 与翻页回写双向联动，`meta.page` 回写可能造成"翻过去被弹回"。
- **M10** BrowserScreen.kt:136 + App.kt:84-89 断开后 serverSummary 不清空。
- **M11** ReaderScreen.kt:85-93 BackHandler 弹层分支实际不可达（ModalBottomSheet 自己处理返回），易误导。

### 低
- L1 App.kt:49-58 启动 scope 未持有（建议收敛为 appScope 并复用于 H1）。
- L2 ApiClient.kt:88-93 未连接时映射为含混的"客户端未就绪"。
- L3 封面宽度 240 硬编码，高密度屏发糊。
- L4 进度 Slider 无无障碍语义。
- L5 Coil 磁盘缓存 1GB 偏激进；release 未开 R8。
- L6 formatSize 对异常值返回空串。
- L7 封面图 contentDescription=null。
- L8 AppNavHost.kt:20 bookId 未 URL 编码。

---

## 三、Python 服务器（按严重程度）

### 高
| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| S1 | config.py:39 + config.yaml | 默认 `host: 0.0.0.0` + 无令牌：局域网任何人可浏览/写整个书库，无速率限制 | 默认 127.0.0.1；0.0.0.0 无令牌时启动醒目告警；按 IP 限流 |
| S2 | api/deps.py:32 | 令牌用普通 `!=` 比较，时序侧信道 | `secrets.compare_digest` |
| S3 | formats/epub.py:253,263,616、fb2.py:54 | lxml 默认 parser 未禁实体：恶意 EPUB/FB2 可借 XXE 读服务器本机文件注入正文，且有实体膨胀风险 | 统一 `XMLParser(resolve_entities=False, no_network=True, load_dtd=False, huge_tree=False)` |

### 中
- **S4** services/progress.py:70-89、library.py:144-167：锁内先清 `_dirty` 再锁外写盘，写盘失败后这批变更要等下次 PUT 才落盘，进程崩溃即丢；且 progress 用固定共享 tmp 路径，并发 save(force) 互相覆盖。→ 写盘成功后再清 dirty；tmp 加唯一后缀。
- **S5** concurrency.py:68-73 + errors.py:74-78：解析槽占满抛 ParseTimeoutError → 422"解析失败"，把"服务器忙"当"书坏了"。→ 独立 503 SERVER_BUSY + Retry-After。
- **S6** epub.py:142-145 资源读取 basename 兜底在多个同名文件时静默串读（EPUB 子目录同名资源常见）。→ 唯一匹配才兜底。
- **S7** routes_books.py:69-92 封面/asset 端点发 ETag 但不处理 If-None-Match（`get_page` 却支持 304），封面列表缓存头形同虚设。
- **S8** routes_files.py + library.py list_dir：每次 /files 全量 scandir 并对未索引文件读 1MB 头算 book_id，可被不同 offset 反复触发重 IO。→ (dir,mtime) TTL 缓存。
- **S9**【复核确认并加重】cache/lru.py:37-58 单条 value 超过 max_bytes 照样插入；且 `_evict` 的 `while self._data` 循环在只剩该大项时仍因超限把它也逐出——**每次请求清空整个缓存（包括它自己）**，命中率完全塌方。→ put 前拒绝 `size > max_bytes`。
- **S10** library.py:608-610 头 1MB 相同的不同文件共享 book_id，`_by_id` 无条件覆盖，后者顶替前者。→ 冲突检测/集合映射。
- **S11** comics.py ZipComicContainer.read 未加锁（RAR/7z 已锁），未来一旦做并发预取即交错读写。→ 补 Lock。
- **S12** routes_progress.py:29 "暂无进度"用 400 BAD_PATH，与错误码表语义冲突。→ 404 NOT_FOUND 或 200+null。
- **S13** cache/store.py:134-144 全局一把锁串行所有书的 PageMap gzip 落盘。→ per-book 锁或只锁 os.replace。
- **S14** library.py:202-234 扫描整体替换索引时，扫描期间旧索引的增量指纹更新被丢弃；替换不触发 content_changed。→ 统一 diff 触发。

### 低
- S15 main.py:77 CancelledError 元组写法冗余且吞异常无日志。
- S16 run.py:147 `background_scan=bool(--no_scan)` 命名反直觉；默认模式无后台增量扫描，新增文件只有浏览目录才发现。
- S17 comics.py:215 规范化后同名条目静默覆盖。
- S18 comics.py:219 包总量校验用包头声明值，谎报时运行期无复查。
- S19 comics.py:668-692 fit=original 强制重编码后未复查字节数。
- S20 textutil.py:223 章节正则会把"第3章…"开头的普通正文行误判为标题。
- S21 search.py:75-88 恰好 500 命中时无法区分"正好"与"被截断"。
- S22 fixed 模式 theme 参数被静默忽略，未在契约中说明。
- S23 /server/info 无鉴权时披露书库名与藏书数（个人藏书枚举面）。
- S24 paths.py:68 Windows 保留名检查误伤"读取"场景（Linux/SMB 上合法的 `con` 目录进不去）。
- S25 cache/parsed.py:112-127 磁盘保存锁内含序列化+gzip。
- S26 pyproject dev 缺 psutil；`.cbz3` 扩展名各处不一致。

---

## 四、修复优先级建议

1. **安全**：S1（默认监听/鉴权告警）→ S3（XXE）→ S2（常量时间比较）→ H5（令牌备份导出）。
2. **正确性**：H1（进度竞态）→ H2（跨页步长失效）→ S4（dirty 时序丢进度）→ S9（大页清空缓存）→ H3（旋转 inset 残留）。
3. **体验**：UI-1（正文被工具栏裁切）→ H4（导航栏遮挡）→ M1（条漫无失败重试）→ M7（主线程 IO）→ UI-3/4（设置面板单位与高度）。
4. **健壮性/契约**：S5（忙≠失败）、S12（进度 404 语义）、M4（重复 key 崩溃）、S6（同名资源串读）。
