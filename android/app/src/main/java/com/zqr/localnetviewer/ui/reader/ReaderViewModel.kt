package com.zqr.localnetviewer.ui.reader

import androidx.compose.ui.unit.IntSize
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import coil3.imageLoader
import coil3.request.ImageRequest
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.ApiException
import com.zqr.localnetviewer.data.net.dto.AnchorDto
import com.zqr.localnetviewer.data.net.dto.ChapterInfoDto
import com.zqr.localnetviewer.data.net.dto.FontInfoDto
import com.zqr.localnetviewer.data.net.dto.SearchResultDto
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.data.prefs.ReadingProgress
import com.zqr.localnetviewer.data.runCatchingIo
import com.zqr.localnetviewer.ui.common.InsetsPx
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt
import java.util.concurrent.ConcurrentHashMap

/**
 * 阅读器状态机（设计 6.4 / 6.5）。
 *
 * 打开顺序：meta → 本地进度 → layout → locate → 渲染起始页并预取。
 * 竞态处理：`/layout` + `/locate` 做 300ms 防抖并取消过期请求；翻页停稳后再取页元数据
 * （300ms 防抖）与保存进度（800ms 防抖）。
 */
class ReaderViewModel(
    app: App,
    private val bookId: String,
) : AndroidViewModel(app) {

    private val libraryRepository = app.libraryRepository
    private val readerRepository = app.readerRepository
    private val settingsStore = app.settingsStore
    private val progressStore = app.progressStore

    data class UiState(
        val bookId: String = "",
        val title: String = "",
        val author: String? = null,
        val format: String = "",
        /** flow | fixed | strip */
        val mode: String = "flow",
        val layoutId: String? = null,
        /** 服务器内容修订号：页面 URL 携带它避免命中旧缓存 */
        val revision: String = "",
        val totalPages: Int = 0,
        val currentPage: Int = 1,
        val chapterTitle: String = "",
        /** 当前页所属章节下标；-1 表示未知（目录高亮用，设计 6.3） */
        val chapterIndex: Int = -1,
        val anchor: AnchorDto? = null,
        val aspect: Float? = null,
        val chapters: List<ChapterInfoDto> = emptyList(),
        val settings: AppSettings = AppSettings.DEFAULT,
        val fonts: List<FontInfoDto> = emptyList(),
        /** 服务器声明支持 JSON 页（`render_support.json`，仅 flow 文本书为 true）。 */
        val renderSupportJson: Boolean = false,
        /** 文字直发模式：页号 → 页面 JSON 状态（服务器渲染模式下恒为空）。 */
        val textPages: Map<Int, TextPageState> = emptyMap(),
        val loading: Boolean = true,
        val preparing: Boolean = false,
        val error: String? = null,
        val errorCode: String? = null,
        val message: String? = null,
        val overlayVisible: Boolean = true,
        val tocOpen: Boolean = false,
        val settingsOpen: Boolean = false,
        val searchOpen: Boolean = false,
        val searchQuery: String = "",
        val searching: Boolean = false,
        val searchResults: List<SearchResultDto> = emptyList(),
        val searchLimited: Boolean = false,
        /** 放大查看时以 scale=2.0 重新渲染当页（只影响渲染，不影响分页） */
        val renderScale: Float = 1f,
    ) {
        val isFixed: Boolean get() = mode != "flow"
        val strip: Boolean get() = mode == "strip"
        val reverseLayout: Boolean
            get() = isFixed && settings.readingDirection == ReadingDirection.RTL.id
        val canRender: Boolean get() = layoutId != null && totalPages > 0

        /** 服务器与当前书籍是否支持文字直发（flow 且服务器支持 JSON 页）。 */
        val textSupported: Boolean get() = mode == "flow" && renderSupportJson

        /**
         * 文字直发是否真正生效：用户选了"文字直发"且书/服务器支持。
         *
         * "尽量"语义：不满足条件时自动回退为服务器渲染的 PNG 页，阅读不中断。
         */
        val textRenderActive: Boolean
            get() = textSupported && settings.pageRenderMode == AppSettings.RENDER_TEXT

        /**
         * 双页跨页时一次翻两页。
         *
         * 服务器 `pair_bytes(page)` 渲染的是 **page 与 page+1**；如果客户端仍按步长 1 翻页，
         * 就会出现 spread1=p1|p2、spread2=p2|p3、spread3=p3|p4 …… 每张内容页被显示两次、
         * 阅读顺序错乱。因此跨页开启时：翻页步长 = 2，且只在奇数页（跨页首页）上取图。
         */
        val spreadStep: Int
            get() = if (isFixed && !strip && settings.pairPages) 2 else 1

        /** 跨页开启时可翻的"屏"数（每屏 2 页）。 */
        val spreadCount: Int
            get() = if (spreadStep == 2) (totalPages + 1) / 2 else totalPages

        /** 真实页码 → Pager 下标（跨页时 1,2 页都指向第 0 屏）。 */
        fun indexOfPage(page: Int): Int =
            ((page - 1).coerceAtLeast(0)) / spreadStep

        /** Pager 下标 → 该屏的首页页码。 */
        fun pageOfIndex(index: Int): Int = index.coerceAtLeast(0) * spreadStep + 1

        val pageLabel: String
            get() = if (totalPages > 0) "第 $currentPage / $totalPages 页" else ""
    }

    private val _state = MutableStateFlow(UiState(bookId = bookId))
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var viewportSize: IntSize? = null
    private var insets: InsetsPx = InsetsPx()
    private var density: Float = 1f
    private var currentSignature: String? = null
    /** 首次布局用于恢复进度的锚点；成功定位一次后即失效，避免之后把读者拉回旧位置 */
    private var restoreAnchor: AnchorDto? = null
    private var restoreRatio: Float? = null
    private var restorePage: Int? = null
    /** 当前 state.anchor 属于哪一页（0 表示未知）：只有等于 currentPage 时才可信 */
    private var anchorPage: Int = 0
    private var metaLoaded = false
    private var initialLayoutStarted = false

    /** 自动重建守卫：文字直发的预取在 IO 域也会触发 [recoverLayout]，必须保证可见性。 */
    @Volatile
    private var recovering = false

    private var relayoutJob: Job? = null
    private var settleJob: Job? = null
    private var progressJob: Job? = null
    private var searchJob: Job? = null

    /** 预取专用：与 viewModelScope 分离，翻页时可单独取消，且限并发（设计 6.4）。 */
    private val prefetchScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val prefetchSlots = Semaphore(PREFETCH_CONCURRENCY)
    private var prefetchJob: Job? = null

    /**
     * 文字直发在途请求集合（页号）：去重与"取消后重取"都依据它，
     * 不能看 UI 状态里的 [TextPageState.Loading]（取消会留下残影）。
     */
    private val textInFlight: MutableSet<Int> = ConcurrentHashMap.newKeySet()

    /** 当前搜索结果对应的 layout_id：重排后旧页码失效，需要用锚点重新定位。 */
    private var searchLayoutId: String? = null

    init {
        viewModelScope.launch {
            val settings = settingsStore.current()
            _state.update { it.copy(settings = settings) }
            // 竞态：首次 /layout 可能先于设置加载完成（用默认设置排了版）。
            // 设置就位后核对签名，不一致就重排，避免整本书一直用默认字号。
            if (initialLayoutStarted) {
                val wanted = currentSignature()
                if (wanted != null && wanted != currentSignature) {
                    scheduleRelayout(300)
                }
            }
        }
        viewModelScope.launch { loadFonts() }
        viewModelScope.launch { openBook() }
    }

    // ------------------------------------------------------------ 打开
    private suspend fun openBook() {
        try {
            val meta = libraryRepository.bookMeta(bookId)
            val progress = progressStore.get(bookId)
            restoreAnchor = progress?.anchor
            restoreRatio = progress?.ratio
            restorePage = progress?.page
            _state.update {
                it.copy(
                    title = meta.title,
                    author = meta.author,
                    format = meta.format,
                    mode = meta.mode,
                    revision = meta.revision,
                    chapters = meta.chapters,
                    renderSupportJson = meta.renderSupport.json,
                    loading = true,
                    error = null,
                    errorCode = null,
                )
            }
            metaLoaded = true
            maybeInitialLayout()
        } catch (error: ApiException) {
            _state.update {
                it.copy(
                    loading = false,
                    error = if (error.isNetwork) {
                        "无法连接服务器：${error.message}"
                    } else {
                        error.message
                    },
                    errorCode = error.code,
                )
            }
        }
    }

    private suspend fun loadFonts() {
        runCatchingIo { readerRepository.fonts() }.onSuccess { response ->
            _state.update { it.copy(fonts = response.fonts) }
        }
    }

    // ------------------------------------------------------------ 视口
    /**
     * 由 UI 上报窗口几何（窗口全尺寸 + 常态 insets + density）。
     *
     * insets 取运行期最大值，因此切换沉浸模式不会触发重新布局（设计 4.6）。
     */
    fun onViewport(viewport: IntSize, insets: InsetsPx, density: Float) {
        if (viewport.width <= 0 || viewport.height <= 0 || density <= 0f) return
        this.viewportSize = viewport
        this.insets = insets
        this.density = density
        maybeInitialLayout()
        if (initialLayoutStarted) {
            val signature = currentSignature()
            if (currentSignature != null && signature != currentSignature) {
                scheduleRelayout(300)
            }
        }
    }

    private fun maybeInitialLayout() {
        if (initialLayoutStarted || !metaLoaded || viewportSize == null) return
        initialLayoutStarted = true
        scheduleRelayout(0)
    }

    private fun currentSignature(): String? {
        val viewport = viewportSize ?: return null
        return profileSignature(viewport, _state.value.settings, density, insets, _state.value.mode)
    }

    /** 300ms 防抖 + 取消过期请求（设计 6.4 控制项防抖与竞态）。 */
    private fun scheduleRelayout(delayMs: Long) {
        relayoutJob?.cancel()
        relayoutJob = viewModelScope.launch {
            if (delayMs > 0) delay(delayMs)
            rebuildLayout()
        }
    }

    private suspend fun rebuildLayout(
        anchorOverride: AnchorDto? = null,
        ratioFallback: Float? = null,
    ) {
        val viewport = viewportSize ?: return
        val snapshot = _state.value
        _state.update { it.copy(preparing = true, error = null, errorCode = null) }
        val profile = buildLayoutProfile(viewport, snapshot.settings, density, insets, snapshot.mode)
        // 记录"本次排版实际使用的输入签名"：设置可能在请求期间变化，不能等响应
        // 回来再按最新 settings 计算，否则会掩盖差异、永远不重排（见 init 的竞态处理）
        val signature = profileSignature(viewport, snapshot.settings, density, insets, snapshot.mode)
        try {
            val layout = readerRepository.registerLayout(bookId, profile)
            currentSignature = signature
            if (layout.totalPages <= 0) {
                // 空文件 / 目录内没有图片：明确提示，避免一直停在加载态
                _state.update {
                    it.copy(
                        layoutId = layout.layoutId,
                        totalPages = 0,
                        mode = layout.mode,
                        currentPage = 1,
                        preparing = false,
                        loading = false,
                        error = "这本书没有可显示的内容（空文件，或图集目录中没有图片）",
                        errorCode = "EMPTY_BOOK",
                    )
                }
                return
            }
            val anchor = anchorOverride ?: sessionAnchor() ?: restoreAnchor
            val fallbackRatio = ratioFallback
                ?: restoreRatio
                ?: ratioFor(snapshot.currentPage, snapshot.totalPages)
            val total = max(1, layout.totalPages)
            val target = when {
                // locate 失败（瞬断/5xx）不能回落到第 1 页：随后 800ms 防抖会把
                // page=1 写成进度，一次网络抖动就永久丢阅读位置。原样抛给
                // handleFatal → ErrorView/重试，恢复锚点保留给下一次重排。
                anchor != null -> runCatchingIo {
                    readerRepository.locate(bookId, layout.layoutId, anchor).page
                }.getOrThrow()
                fallbackRatio != null && fallbackRatio > 0f -> {
                    (fallbackRatio * layout.totalPages).roundToInt().coerceAtLeast(1)
                }
                restorePage != null -> restorePage ?: 1
                else -> 1
            }
            // 恢复锚点只用一次：定位成功后再出错也不能把读者拉回旧位置
            restoreAnchor = null
            restoreRatio = null
            val chapters = if (layout.chapters.isNotEmpty()) layout.chapters else snapshot.chapters
            _state.update {
                it.copy(
                    layoutId = layout.layoutId,
                    revision = layout.revision,
                    totalPages = layout.totalPages,
                    mode = layout.mode,
                    chapters = chapters,
                    currentPage = target.coerceIn(1, total),
                    // 新 layout 下旧页 JSON 的页码/内容都不再对应，整体作废
                    textPages = emptyMap(),
                    preparing = false,
                    loading = false,
                    error = null,
                    errorCode = null,
                )
            }
            onPageSettled(_state.value.currentPage, immediate = true)
        } catch (cancelled: CancellationException) {
            // 防抖取消是正常流程（连续改字号/旋转屏幕），不能当成错误展示
            throw cancelled
        } catch (error: ApiException) {
            handleFatal(error)
        } catch (error: Exception) {
            handleFatal(ApiException(ApiException.CODE_INTERNAL, error.message ?: "排版失败"))
        }
    }

    private fun handleFatal(error: ApiException) {
        val text = if (error.isNetwork) "无法连接服务器：${error.message}" else error.message
        _state.update {
            it.copy(
                preparing = false,
                loading = false,
                error = text,
                errorCode = error.code,
                // 已有可读页面时 ErrorView 不会显示（仅 !canRender 时展示），
                // 改用消息通道/Snackbar 提示重排失败，保留当前页继续阅读
                message = if (it.canRender) text else it.message,
            )
        }
    }

    /** 重试：meta 未就绪则重开书籍；layout 未就绪则重新排版；否则重取当前页。 */
    fun retry() {
        val snapshot = _state.value
        when {
            !metaLoaded -> {
                _state.update { it.copy(loading = true, error = null) }
                viewModelScope.launch { openBook() }
            }
            // 空书（EMPTY_BOOK）重试没有意义：文件没变，重排结果必然还是 0 页，
            // 只会让用户陷入"重试 → 仍为空 → 再报错"的死循环。改为重新读 meta，
            // 这样用户在服务器端补了内容之后依然能恢复。
            snapshot.errorCode == "EMPTY_BOOK" -> {
                metaLoaded = false
                _state.update { it.copy(loading = true, error = null, errorCode = null) }
                viewModelScope.launch { openBook() }
            }
            snapshot.layoutId == null -> {
                _state.update { it.copy(preparing = true, error = null) }
                scheduleRelayout(0)
            }
            else -> {
                _state.update { it.copy(error = null) }
                onPageSettled(snapshot.currentPage, immediate = true)
            }
        }
    }

    // ------------------------------------------------------------ 翻页
    /** 翻页停稳后调用（Pager settledPage 或条漫首个可见项）。 */
    fun onPageSettled(page: Int, immediate: Boolean = false) {
        if (page < 1) return
        _state.update { if (it.currentPage == page) it else it.copy(currentPage = page) }
        // 文字直发：当前页立即取 JSON（不等 300ms 防抖；预取已命中则这里直接跳过）
        if (_state.value.textRenderActive) requestTextPage(page)
        settleJob?.cancel()
        settleJob = viewModelScope.launch {
            if (!immediate) delay(SETTLE_DEBOUNCE_MS)
            fetchPageMeta(page)
            prefetchAround(page)
            scheduleProgressSave(page)
        }
        // 重排失败（离线等）后 currentSignature 会一直与新设置不匹配；阅读页没有
        // 可见的重试入口，用户每次翻页时补一次重排尝试，恢复在线后自动收敛
        val wanted = currentSignature()
        if (wanted != null && wanted != currentSignature &&
            relayoutJob?.isActive != true && !recovering
        ) {
            scheduleRelayout(300)
        }
    }

    private suspend fun fetchPageMeta(page: Int) {
        val layoutId = _state.value.layoutId ?: return
        try {
            val meta = readerRepository.pageMeta(bookId, page, layoutId, _state.value.revision)
            // 过期响应防护：等待期间用户可能已经翻页或重排（onPageImageFailed 的
            // 恢复请求不受 settleJob 取消约束），把 currentPage 拉回旧页会打断阅读
            if (_state.value.layoutId != layoutId || _state.value.currentPage != page) return
            // 锚点必须和它所属的页一起记录，否则翻页后立刻退出会把"新页码 + 旧锚点"存下来
            anchorPage = meta.page
            _state.update {
                it.copy(
                    anchor = meta.anchor,
                    chapterTitle = meta.chapterTitle,
                    chapterIndex = meta.chapterIndex,
                    aspect = meta.aspect,
                    currentPage = meta.page,
                )
            }
        } catch (error: ApiException) {
            when {
                error.isLayoutLost -> recoverLayout("布局缓存已失效，正在自动重建")
                error.isPageOutOfRange -> recoverLayout("内容已变化，正在刷新")
                // 错误体缺失/不可解析时 404 会退化成 NOT_FOUND（不再误报 BOOK_NOT_FOUND）：
                // 对页元数据而言最可能的原因仍是 layout 已失效，走同一恢复路径
                error.code == ApiException.CODE_NOT_FOUND ->
                    recoverLayout("布局缓存已失效，正在自动重建")
                // 阅读页没有下拉刷新（只有左右翻页），文案不能承诺做不到的操作
                error.isNetwork -> _state.update { it.copy(message = "网络中断：${error.message}") }
                else -> _state.update { it.copy(message = error.message) }
            }
        }
    }

    /**
     * 服务器重启 / 缓存淘汰后的自动恢复（设计 6.4：用户无感）。
     *
     * 文字直发的失败路径可能在 IO 域（预取）触发恢复，而 [relayoutJob] / [recovering]
     * 与 scheduleRelayout 共享：统一切到主线程，保证守卫与任务句柄的单线程读改写。
     */
    private fun recoverLayout(message: String) {
        viewModelScope.launch {
            if (recovering) return@launch
            recovering = true
            // 与 scheduleRelayout 共用同一个 relayoutJob：用户改设置/旋转触发的重排
            // 会取消正在进行的恢复，避免两份不同参数的 /layout 响应互相覆盖
            relayoutJob?.cancel()
            relayoutJob = viewModelScope.launch {
                val snapshot = _state.value
                val ratio = ratioFor(snapshot.currentPage, snapshot.totalPages)
                _state.update { it.copy(message = message) }
                try {
                    rebuildLayout(
                        anchorOverride = sessionAnchor() ?: restoreAnchor,
                        ratioFallback = ratio,
                    )
                } finally {
                    recovering = false
                }
            }
        }
    }

    fun jumpToPage(page: Int) {
        val total = max(1, _state.value.totalPages)
        val step = _state.value.spreadStep
        // 跨页时对齐到"跨页首页"（奇数页），否则下一页会漏掉或重复一整屏
        val aligned = if (step == 2 && page % 2 == 0) page - 1 else page
        val target = aligned.coerceIn(1, total)
        _state.update { it.copy(currentPage = target, tocOpen = false, searchOpen = false) }
        onPageSettled(target, immediate = true)
    }

    fun nextPage() {
        jumpToPage(_state.value.currentPage + _state.value.spreadStep)
    }

    fun previousPage() {
        jumpToPage(_state.value.currentPage - _state.value.spreadStep)
    }

    fun jumpToChapter(chapterIndex: Int) {
        val chapter = _state.value.chapters.firstOrNull { it.index == chapterIndex }
        val page = chapter?.startPage
        if (page == null || page <= 0) {
            _state.update { it.copy(message = "该章节暂无页码，请稍后重试") }
            return
        }
        jumpToPage(page)
    }

    // ------------------------------------------------------------ 预取
    /**
     * 当前页稳定后后台预取 ±1、±2 页（设计 6.4）。
     *
     * 设计对预取有三条明确要求，这里逐条落实：
     * 1. **低优先级、限并发 2–3** —— 用独立的 [prefetchSlots] 信号量限流，
     *    避免四个预取把 Coil 的请求队列塞满、把当前页挤到后面；
     * 2. **当前页优先** —— 当前页由 Pager 自己的 AsyncImage 发起，不经过这里；
     * 3. **翻页时取消尚未开始的预取** —— 每次翻页 `cancel()` 上一个预取任务，
     *    已在传输中的请求交给 Coil 的缓存去重。
     */
    private fun prefetchAround(page: Int) {
        val snapshot = _state.value
        if (snapshot.layoutId == null || snapshot.totalPages <= 0) return
        // 文字直发：预取 JSON（体积小），不再为同一页拉 PNG 图片；
        // 与 PNG 预取共用同一个 job + 信号量：翻页取消未开始的预取、限并发 2
        if (snapshot.textRenderActive) {
            val targets = ArrayList<Int>(4)
            for (delta in intArrayOf(1, -1, 2, -2)) {
                val target = page + delta
                if (target < 1 || target > snapshot.totalPages) continue
                targets.add(target)
            }
            prefetchJob?.cancel()
            if (targets.isEmpty()) return
            prefetchJob = prefetchScope.launch {
                for (target in targets) {
                    prefetchSlots.withPermit { prefetchTextPage(target) }
                }
            }
            return
        }
        val app = getApplication<App>()
        val loader = app.imageLoader
        // 与屏上 AsyncImage 相同的目标尺寸（视口像素）：Coil 内存缓存键包含 size，
        // 不指定会另存一份解码结果，且可能按原图超采样（scale=2.0 时尤其浪费）
        val viewport = viewportSize
        // 跨页模式下 Pager 的每一屏都是奇数页（步长 2），所以 ±1 页根本不会被显示
        // （page+1 是当前屏的第二页、page-1 是上一屏的首页），预取要按步长走
        val step = snapshot.spreadStep
        val targets = ArrayList<String>(4)
        for (delta in intArrayOf(step, -step, 2 * step, -2 * step)) {
            val target = page + delta
            if (target < 1 || target > snapshot.totalPages) continue
            pageUrlFor(target, snapshot)?.let { if (!targets.contains(it)) targets.add(it) }
        }
        prefetchJob?.cancel()
        if (targets.isEmpty()) return
        prefetchJob = prefetchScope.launch {
            for (url in targets) {
                prefetchSlots.withPermit {
                    // 预取失败无所谓（页面切过去时还会再请求一次）；
                    // 但取消必须透传：runCatching 会把 CancellationException 吞成
                    // 普通失败，靠"下一次挂起点再抛"只是巧合行为（项目里统一用
                    // runCatchingIo 正是为此）
                    runCatchingIo {
                        val request = ImageRequest.Builder(app).data(url)
                        if (viewport != null && viewport.width > 0 && viewport.height > 0) {
                            request.size(viewport.width, viewport.height)
                        }
                        loader.execute(request.build())
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ 文字直发
    /**
     * 取一页 JSON（设计 3.2）：当前页 / 失败重试走这里（ViewModel 域）。
     *
     * 预取走 [prefetchTextPage]（独立 scope + 信号量，翻页可取消）。
     */
    private fun requestTextPage(page: Int, force: Boolean = false) {
        if (!startTextPageFetch(page, force)) return
        viewModelScope.launch { fetchTextPage(page) }
    }

    /** 后台预取一页 JSON：失败静默（翻到该页时会重新请求）。 */
    private suspend fun prefetchTextPage(page: Int) {
        if (!startTextPageFetch(page)) return
        fetchTextPage(page)
    }

    /**
     * 登记"该页开始加载"。
     *
     * 去重依据是 [textInFlight]（真实在途请求）而不是 UI 里的 [TextPageState.Loading]：
     * 预取任务被翻页取消时状态可能残留 Loading，若拿它当"在途"就会永久拒绝重取
     * （页面无限转圈且没有重试入口）。页码越界、layout 未就绪、[force] 场景外已有
     * 内容时一律不取。
     */
    private fun startTextPageFetch(page: Int, force: Boolean = false): Boolean {
        val snapshot = _state.value
        if (!snapshot.textRenderActive) return false
        if (page < 1 || (snapshot.totalPages > 0 && page > snapshot.totalPages)) return false
        if (snapshot.layoutId == null) return false
        if (!force && snapshot.textPages[page] is TextPageState.Ready) return false
        if (!textInFlight.add(page)) return false
        _state.update { state ->
            state.copy(
                textPages = pruneTextPages(
                    state.textPages + (page to TextPageState.Loading),
                    state.currentPage,
                )
            )
        }
        return true
    }

    /**
     * 拉取并落状态。
     *
     * - 响应回来时 layout 已重建 / 已切回服务器渲染则丢弃；若该页仍是当前页，
     *   立即按新 layout 重新取（否则重排后回到当前页会只剩一个永远转圈的占位）；
     * - 预取被取消时清掉 Loading 残影并做同样的重取；
     * - layout 失效 / 页码越界与 PNG 模式同路径：触发自动重建，用户无感。
     */
    private suspend fun fetchTextPage(page: Int) {
        val layoutId = _state.value.layoutId ?: run {
            textInFlight.remove(page)
            return
        }
        try {
            val pageJson = readerRepository.flowPage(bookId, page, layoutId)
            textInFlight.remove(page)
            val now = _state.value
            if (now.layoutId != layoutId || !now.textRenderActive) {
                reRequestTextPageIfCurrent(page, now)
                return
            }
            _state.update { state ->
                state.copy(
                    textPages = pruneTextPages(
                        state.textPages + (page to TextPageState.Ready(pageJson)),
                        state.currentPage,
                    )
                )
            }
        } catch (cancelled: CancellationException) {
            textInFlight.remove(page)
            _state.update { state ->
                if (state.textPages[page] is TextPageState.Loading) {
                    state.copy(textPages = state.textPages - page)
                } else {
                    state
                }
            }
            reRequestTextPageIfCurrent(page, _state.value)
            throw cancelled
        } catch (error: ApiException) {
            textInFlight.remove(page)
            val now = _state.value
            if (now.layoutId != layoutId || !now.textRenderActive) {
                reRequestTextPageIfCurrent(page, now)
                return
            }
            when {
                error.isLayoutLost || error.isPageOutOfRange ||
                    error.code == ApiException.CODE_NOT_FOUND -> {
                    // 不直接删条目：自动重建失败（断网）时删除会退化成"永远转圈、没有重试入口"。
                    // 留给 Failed 占位；重建成功会整体清空 textPages 并自动重取。
                    _state.update { state ->
                        state.copy(
                            textPages = pruneTextPages(
                                state.textPages + (
                                    page to TextPageState.Failed("排版已失效，正在自动重建")
                                ),
                                state.currentPage,
                            )
                        )
                    }
                    recoverLayout("布局缓存已失效，正在自动重建")
                }
                else -> _state.update { state ->
                    state.copy(
                        textPages = pruneTextPages(
                            state.textPages + (page to TextPageState.Failed(error.message)),
                            state.currentPage,
                        )
                    )
                }
            }
        } catch (error: Exception) {
            // 预取在 IO 域运行：非 ApiException 不能把协程炸掉；过期 layout 的失败同样丢弃
            textInFlight.remove(page)
            val now = _state.value
            if (now.layoutId != layoutId || !now.textRenderActive) {
                reRequestTextPageIfCurrent(page, now)
                return
            }
            _state.update { state ->
                state.copy(
                    textPages = pruneTextPages(
                        state.textPages + (page to TextPageState.Failed(error.message ?: "取页失败")),
                        state.currentPage,
                    )
                )
            }
        }
    }

    /**
     * 作废的响应（旧 layout / 已切模式）丢弃后，若该页还是当前页则用新 layout 重取。
     *
     * 在途请求不能阻塞重排后的重取：重排会清空 [UiState.textPages]，而重排完成时
     * `onPageSettled` 的请求会被在途去重挡下，必须由这里补一次。
     */
    private fun reRequestTextPageIfCurrent(page: Int, snapshot: UiState) {
        if (snapshot.textRenderActive && snapshot.currentPage == page) {
            requestTextPage(page)
        }
    }

    /** 文字直发模式下的整页重试（页面失败占位上调用）。 */
    fun retryTextPage(page: Int) {
        requestTextPage(page, force = true)
    }

    /** 页缓存上限：只保留离当前页最近的 N 页，避免长时间阅读把整本 JSON 攒在内存里。 */
    private fun pruneTextPages(
        pages: Map<Int, TextPageState>,
        currentPage: Int,
    ): Map<Int, TextPageState> {
        if (pages.size <= TEXT_CACHE_PAGES) return pages
        return pages.entries
            .sortedBy { abs(it.key - currentPage) }
            .take(TEXT_CACHE_PAGES)
            .associate { it.key to it.value }
    }

    // ------------------------------------------------------------ URL
    fun pageUrl(page: Int): String? = pageUrlFor(page, _state.value)

    private fun pageUrlFor(page: Int, snapshot: UiState): String? {
        val layoutId = snapshot.layoutId ?: return null
        if (page < 1 || (snapshot.totalPages > 0 && page > snapshot.totalPages)) return null
        val fixed = snapshot.isFixed
        val fit = if (snapshot.settings.originalQuality && !snapshot.strip) "original" else "viewport"
        return readerRepository.pageUrl(
            bookId = bookId,
            page = page,
            layoutId = layoutId,
            theme = snapshot.settings.theme,
            scale = snapshot.renderScale,
            fit = fit,
            quality = 85,
            pair = snapshot.settings.pairPages && fixed && !snapshot.strip,
            rtl = snapshot.reverseLayout,
            fixed = fixed,
            revision = snapshot.revision,
        )
    }

    // ------------------------------------------------------------ 设置
    /**
     * 阅读设置变化。
     *
     * 只有几何相关字段（字号 / 行距 / 边距 / 段间距 / 字体 / 条漫）才重新 `/layout`；
     * 主题、适应模式、原图、方向、跨页只改客户端显示或 URL，不触发重新分页（设计 6.4）。
     */
    fun updateSettings(new: AppSettings) {
        val old = _state.value.settings
        _state.update { it.copy(settings = new) }
        // 切换双页跨页会改变 spreadStep（Pager 下标 ↔ 页码映射），必须把
        // currentPage 对齐到新步长的"屏首页"：否则 61|62 关跨页后 index 不变、
        // 页码变成 31，并把错误页码写进进度；开启时则成倍前跳。
        // （pairPages 不进几何签名，不会触发重新分页。）
        val aligned = _state.value.let { it.pageOfIndex(it.indexOfPage(it.currentPage)) }
        if (aligned != _state.value.currentPage) {
            _state.update { it.copy(currentPage = aligned) }
            onPageSettled(aligned, immediate = true)
        }
        // 落盘失败不影响本次阅读（内存中的设置已生效）
        viewModelScope.launch { runCatchingIo { settingsStore.update { new } } }
        // 页面模式切换：旧模式的页数据（JSON / PNG URL）都不再适用
        if (old.pageRenderMode != new.pageRenderMode) {
            // 预取只服务于旧模式：继续跑只会把结果写进已清空的缓存
            prefetchJob?.cancel()
            _state.update { it.copy(textPages = emptyMap()) }
            // 切到文字直发需要立刻取当前页；切回服务器渲染由 URL 变化自然重载
            if (_state.value.textRenderActive) {
                onPageSettled(_state.value.currentPage, immediate = true)
            }
        }
        val geometryChanged = old.geometrySignature() != new.geometrySignature()
        if (geometryChanged) {
            scheduleRelayout(300)
        }
    }

    /** 双击放大：以 scale=2.0 重新请求当页，保证放大后文字清晰（设计 6.4）。 */
    fun setZoomed(zoomed: Boolean) {
        val target = if (zoomed) 2f else 1f
        if (_state.value.renderScale == target) return
        _state.update { it.copy(renderScale = target) }
    }

    // ------------------------------------------------------------ 界面状态
    fun toggleOverlay() {
        _state.update { it.copy(overlayVisible = !it.overlayVisible) }
    }

    fun openToc() {
        _state.update { it.copy(tocOpen = true) }
    }

    fun closeToc() {
        _state.update { it.copy(tocOpen = false) }
    }

    fun openSettings() {
        _state.update { it.copy(settingsOpen = true) }
    }

    fun closeSettings() {
        _state.update { it.copy(settingsOpen = false) }
    }

    fun openSearch() {
        _state.update { it.copy(searchOpen = true, overlayVisible = true) }
    }

    fun closeSearch() {
        _state.update { it.copy(searchOpen = false) }
    }

    fun consumeMessage() {
        _state.update { it.copy(message = null) }
    }

    /** 供界面提示（页面加载失败等）。 */
    fun notify(message: String) {
        _state.update { it.copy(message = message) }
    }

    /**
     * 当前页图片加载失败（UI 上报）。
     *
     * 同 URL 内容变化时服务器会失效 layout：图片先失败，页元数据请求若晚于
     * 失效时间会返回 LAYOUT_NOT_FOUND 并触发自动重建（不新增服务器端点）。
     * 没有这一步，用户会停在失败占位上反复重试同一张已失效的图片。
     */
    fun onPageImageFailed(message: String) {
        _state.update { it.copy(message = message) }
        if (_state.value.layoutId == null) return
        val page = _state.value.currentPage
        viewModelScope.launch { fetchPageMeta(page) }
    }

    // ------------------------------------------------------------ 搜索
    fun runSearch(query: String) {
        val keyword = query.trim()
        if (keyword.isEmpty()) return
        // 取消上一次未完成的搜索：全文搜索可能很慢，旧查询若晚于新查询返回，
        // 会把新关键词的结果覆盖成旧结果（用户看到"搜了 B 却出现 A 的命中"）
        searchJob?.cancel()
        searchJob = viewModelScope.launch {
            _state.update {
                it.copy(
                    searching = true,
                    searchQuery = keyword,
                    searchResults = emptyList(),
                    searchLimited = false,
                )
            }
            try {
                val layoutId = _state.value.layoutId
                val response = readerRepository.search(bookId, keyword, layoutId)
                // launch 被取消时这里不会执行；再核对一次关键词，双保险
                if (_state.value.searchQuery != keyword) return@launch
                // 记录结果对应的 layout：之后若发生重排，openSearchResult 改用锚点定位
                searchLayoutId = layoutId
                _state.update {
                    it.copy(
                        searching = false,
                        searchResults = response.results,
                        searchLimited = response.limited,
                        message = if (response.total == 0) "未找到匹配内容" else null,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                if (_state.value.searchQuery != keyword) return@launch
                _state.update {
                    it.copy(
                        searching = false,
                        message = if (error.code == "UNSUPPORTED_FORMAT") {
                            "该格式暂不支持全文搜索"
                        } else {
                            error.message
                        },
                    )
                }
            }
        }
    }

    fun openSearchResult(item: SearchResultDto) {
        val snapshot = _state.value
        val currentLayout = snapshot.layoutId
        val page = item.page
        if (page != null && page > 0 && searchLayoutId == currentLayout) {
            jumpToPage(page)
            _state.update { it.copy(searchOpen = false) }
            return
        }
        // 搜索后发生过重排：旧 page 属于旧 layout_id，直接跳会落到错误的页。
        // 结果里带锚点，用当前 layout 重新 /locate 定位。
        if (currentLayout != null) {
            _state.update { it.copy(searchOpen = false, message = "排版已更新，正在定位…") }
            viewModelScope.launch {
                runCatchingIo {
                    readerRepository.locate(
                        bookId,
                        currentLayout,
                        AnchorDto(chapter = item.chapterIndex, block = item.block, char = item.char),
                    ).page
                }.onSuccess { jumpToPage(it) }
                    .onFailure { _state.update { s -> s.copy(message = "定位失败：${it.message}") } }
            }
            return
        }
        _state.update { it.copy(message = "该结果暂无页码，请先加载排版") }
    }

    // ------------------------------------------------------------ 进度
    private fun scheduleProgressSave(page: Int) {
        progressJob?.cancel()
        progressJob = viewModelScope.launch {
            delay(PROGRESS_DEBOUNCE_MS)
            persistProgress(page)
        }
    }

    /** onStop 时立即落盘（设计 6.5）。 */
    fun saveProgressNow() {
        val snapshot = _state.value
        // 首次排版尚未完成（totalPages=0，currentPage 还是默认 1）时不能落盘：
        // 在打开大书/图集的过程中按 Home/切后台会把旧进度覆盖成第 1 页。
        // 重排期间 totalPages 仍是旧值、currentPage 仍然有效，可以正常保存。
        if (snapshot.layoutId == null || snapshot.totalPages <= 0) return
        viewModelScope.launch { persistProgress(snapshot.currentPage) }
    }

    /**
     * ViewModel 被销毁时兜底落盘（设计 6.5）。
     *
     * `viewModelScope` 在 onCleared 之后不再接受新任务，所以要另起一个与进程生命周期
     * 同级的 scope：用户"翻页后 800ms 内退出阅读页"时，防抖任务会随 ViewModel 一起取消，
     * 只有 ON_STOP 一条兜底；返回键退出 / 系统回收可能都收不到 ON_STOP。
     */
    override fun onCleared() {
        val page = _state.value.currentPage
        val anchor = if (anchorPage == page) _state.value.anchor else null
        val total = _state.value.totalPages
        val ratio = ratioFor(page, total) ?: 0f
        prefetchJob?.cancel()
        prefetchScope.cancel()
        if (total <= 0) {
            return
        }
        // 兜底落盘：viewModelScope 已不可用，交给进程级 scope；写入顺序由
        // ProgressStore 保证（pending Job + get/flow 先等待），因此"退出后立刻
        // 重开"仍能读到最新位置。这里再有界等待 300ms：正常 DataStore 写入毫秒级
        // 完成，保证进程被立刻杀掉前已落盘；磁盘异常时也不会卡死退出路径。
        val job = progressStore.putAsync(
            getApplication<App>().applicationScope,
            bookId,
            ReadingProgress(anchor = anchor, page = page, ratio = ratio),
        )
        runBlocking { withTimeoutOrNull(300L) { job.join() } }
    }

    /** 会话内锚点：只有它确实属于当前页时才可信（否则用比例恢复）。 */
    private fun sessionAnchor(): AnchorDto? =
        if (anchorPage == _state.value.currentPage) _state.value.anchor else null

    /**
     * 页 → 比例的唯一换算（修复两套口径漂移）：用"页中心"
     * ``(page - 0.5) / total``，保存进度、重排定位（/locate 兜底）共用；
     * roundToInt 后恰好回到原页，任何路径写出的 ratio 都一致。
     */
    private fun ratioFor(page: Int, total: Int): Float? =
        if (total > 0) ((page - 0.5f) / total).coerceIn(0f, 1f) else null

    private suspend fun persistProgress(page: Int) {
        val snapshot = _state.value
        // 只有锚点属于要保存的这一页时才记录；否则只存页码+比例，避免"错锚点"把位置带偏
        val anchor = if (anchorPage == page) snapshot.anchor else null
        val ratio = ratioFor(page, snapshot.totalPages) ?: 0f
        // 进度是"尽力而为"的本地数据：写失败（磁盘满等）不能影响阅读，也不能抛出崩溃
        runCatchingIo {
            progressStore.put(
                bookId,
                ReadingProgress(
                    anchor = anchor,
                    page = page,
                    ratio = ratio.coerceIn(0f, 1f),
                ),
            )
        }
    }

    private companion object {
        const val SETTLE_DEBOUNCE_MS = 300L
        const val PROGRESS_DEBOUNCE_MS = 800L

        /** 预取并发上限（设计 6.4 要求 2–3）。 */
        const val PREFETCH_CONCURRENCY = 2

        /** 文字直发页缓存上限（页号离当前页最近优先保留）。 */
        const val TEXT_CACHE_PAGES = 12
    }
}
