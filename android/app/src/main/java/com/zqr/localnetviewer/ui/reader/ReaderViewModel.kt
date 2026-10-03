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
import kotlin.math.max
import kotlin.math.roundToInt

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
    private var recovering = false

    private var relayoutJob: Job? = null
    private var settleJob: Job? = null
    private var progressJob: Job? = null
    private var searchJob: Job? = null

    /** 预取专用：与 viewModelScope 分离，翻页时可单独取消，且限并发（设计 6.4）。 */
    private val prefetchScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val prefetchSlots = Semaphore(PREFETCH_CONCURRENCY)
    private var prefetchJob: Job? = null

    init {
        viewModelScope.launch {
            val settings = settingsStore.current()
            _state.update { it.copy(settings = settings) }
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
                    chapters = meta.chapters,
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
        runCatching { readerRepository.fonts() }.onSuccess { response ->
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
        try {
            val layout = readerRepository.registerLayout(bookId, profile)
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
                anchor != null -> runCatching {
                    readerRepository.locate(bookId, layout.layoutId, anchor).page
                }.getOrDefault(1)
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
                    totalPages = layout.totalPages,
                    mode = layout.mode,
                    chapters = chapters,
                    currentPage = target.coerceIn(1, total),
                    preparing = false,
                    loading = false,
                    error = null,
                    errorCode = null,
                )
            }
            currentSignature = currentSignature()
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
        _state.update {
            it.copy(
                preparing = false,
                loading = false,
                error = if (error.isNetwork) "无法连接服务器：${error.message}" else error.message,
                errorCode = error.code,
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
        settleJob?.cancel()
        settleJob = viewModelScope.launch {
            if (!immediate) delay(SETTLE_DEBOUNCE_MS)
            fetchPageMeta(page)
            prefetchAround(page)
            scheduleProgressSave(page)
        }
    }

    private suspend fun fetchPageMeta(page: Int) {
        val layoutId = _state.value.layoutId ?: return
        try {
            val meta = readerRepository.pageMeta(bookId, page, layoutId)
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
                // 阅读页没有下拉刷新（只有左右翻页），文案不能承诺做不到的操作
                error.isNetwork -> _state.update { it.copy(message = "网络中断：${error.message}") }
                else -> _state.update { it.copy(message = error.message) }
            }
        }
    }

    /** 服务器重启 / 缓存淘汰后的自动恢复（设计 6.4：用户无感）。 */
    private fun recoverLayout(message: String) {
        if (recovering) return
        recovering = true
        val snapshot = _state.value
        val ratio = ratioFor(snapshot.currentPage, snapshot.totalPages)
        _state.update { it.copy(message = message) }
        viewModelScope.launch {
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
        val app = getApplication<App>()
        val loader = app.imageLoader
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
                        loader.execute(ImageRequest.Builder(app).data(url).build())
                    }
                }
            }
        }
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
        // 整体覆盖写入：new 是包含全部字段的完整设置（这里不使用变换参数）
        // 落盘失败不影响本次阅读（内存中的设置已生效）
        viewModelScope.launch { runCatching { settingsStore.update { new } } }
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

    fun showOverlay() {
        _state.update { it.copy(overlayVisible = true) }
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
        val page = item.page
        if (page != null && page > 0) {
            jumpToPage(page)
            _state.update { it.copy(searchOpen = false) }
        } else {
            _state.update { it.copy(message = "该结果暂无页码，请先加载排版") }
        }
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
        val page = _state.value.currentPage
        viewModelScope.launch { persistProgress(page) }
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
        // 同步兜底落盘：viewModelScope 已不可用，异步写入会与"立刻重新打开同一本书"
        // 的新实例读取竞态（旧页码晚到覆盖新会话）。onCleared 在旧实例销毁、新实例
        // 创建之前执行，同步写完即保证了先后次序；ProgressStore 内部再按 bookId
        // 串行化。DataStore 单条写入很快，限时 2 秒兜底，绝不卡死退出路径。
        val store = progressStore
        val id = bookId
        runBlocking {
            withTimeoutOrNull(2000) {
                runCatchingIo {
                    store.put(id, ReadingProgress(anchor = anchor, page = page, ratio = ratio))
                }
            }
        }
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
        runCatching {
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
    }
}
