package com.zqr.localnetviewer.ui.browser

import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.ApiException
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.data.net.dto.GalleryInfoDto
import com.zqr.localnetviewer.data.prefs.RecentBook
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * 浏览页状态机（设计 6.3 BrowserScreen）。
 *
 * 目录优先 + 自然排序由服务器完成；列表按页显示（页码制分页，服务端 offset/limit），
 * 这里负责翻页/跳页、"打开为图集"入口与错误提示。
 */
class BrowserViewModel(app: App) : AndroidViewModel(app) {

    private val repository = app.libraryRepository
    private val appRef = app

    data class UiState(
        val path: String = "/",
        val parent: String? = null,
        val gallery: GalleryInfoDto? = null,
        val entries: List<FileEntryDto> = emptyList(),
        val total: Int = 0,
        /** 当前页（1 基）。 */
        val page: Int = 1,
        val loading: Boolean = true,
        val opening: Boolean = false,
        val scanning: Boolean = false,
        val error: String? = null,
        val message: String? = null,
        /** 已连接服务器概要（原名 App.serverSummary，移到这里避免 Composable 直接读 App）。 */
        val serverSummary: String = "",
        /** 目录历史栈深度：> 0 时可"返回上一页"（恢复离开时的页码与滚动位置）。 */
        val backStackDepth: Int = 0,
        /**
         * 成功加载数据的版本号（每次成功 +1）。UI 用它区分"新数据到达"（需要
         * 回顶/恢复滚动）与"从阅读页返回重建组合"（版本没变，保持原有位置）。
         */
        val dataVersion: Int = 0,
        /** 最近阅读（继续阅读卡片），按落盘时间倒序。 */
        val recent: List<RecentBook> = emptyList(),
        /** 当前列表里各书的已读比例（book_id → 0..1），用于列表行的细进度条。 */
        val progressById: Map<String, Float> = emptyMap(),
        /** 当前服务器可见的书库名（只有根目录响应带；子目录沿用上一次）。 */
        val libraryNames: List<String> = emptyList(),
        /** 书库搜索面板。 */
        val searchOpen: Boolean = false,
        val searchQuery: String = "",
        val searching: Boolean = false,
        val searchResults: List<FileEntryDto> = emptyList(),
        val searchTotal: Int = 0,
        val searchTruncated: Boolean = false,
        /** 搜索时书库仍在扫描：结果可能不全，界面要明说。 */
        val searchScanning: Boolean = false,
        /** 搜索失败原因（网络/服务端错误）：不能与"没有匹配"混为一谈。 */
        val searchError: String? = null,
    ) {
        val totalPages: Int get() = totalPagesOf(total)

        val canOpenCurrentAsGallery: Boolean
            get() = gallery?.canOpenAsBook == true

        val currentName: String
            get() = if (path == "/") "书库" else path.trimEnd('/').substringAfterLast('/')
    }

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** 需要打开的书（点书籍、或把文件夹注册为图集后）。 */
    private val _openBook = MutableStateFlow<String?>(null)
    val openBook: StateFlow<String?> = _openBook.asStateFlow()

    /** 要在内嵌播放器打开的媒体（M5：音频/视频条目）。 */
    data class MediaTarget(val url: String, val title: String, val audioOnly: Boolean)

    private val _openMedia = MutableStateFlow<MediaTarget?>(null)
    val openMedia: StateFlow<MediaTarget?> = _openMedia.asStateFlow()

    private var loadJob: Job? = null
    private var searchJob: Job? = null

    /** 目录访问历史：进入新目录时压入离开处的（目录, 页码, 滚动位置），返回时弹出恢复。 */
    private class HistoryEntry(val path: String, val page: Int, val scrollIndex: Int, val scrollOffset: Int)

    private val history = ArrayDeque<HistoryEntry>()

    /** 待恢复的滚动位置（返回上一页时设置，UI 在新数据渲染后取走并清空）。非 Compose 状态。 */
    class ScrollRestore(val index: Int, val offset: Int)

    @Volatile
    private var pendingScrollRestore: ScrollRestore? = null

    /** 已把滚动指令交给 UI 的 dataVersion（从阅读页返回重建组合时，effect 会重跑，靠它去重）。 */
    @Volatile
    private var scrollHandledVersion = 0

    // 当前列表滚动位置由 UI 持续上报（不用 Compose 状态，避免每帧滚动触发全页重组），
    // 离开目录时用它快照，返回时才能恢复到原位
    @Volatile
    private var curScrollIndex = 0

    @Volatile
    private var curScrollOffset = 0

    /** UI 每次列表滚动后上报位置（首可见项下标 + 像素偏移）。 */
    fun updateScroll(index: Int, offset: Int) {
        curScrollIndex = index
        curScrollOffset = offset
    }

    /**
     * UI 在数据版本 [version] 渲染后领取滚动指令：返回"恢复位置"（返回上一页）或
     * null（回顶部）。同一版本只发一次——Navigation 在进阅读页后会销毁本页组合、
     * 返回时重建，LaunchedEffect 会重新执行；版本未变时必须返回"无需滚动"，
     * 让 rememberSaveable 恢复的位置得以保留。
     */
    fun pendingScrollAction(version: Int): ScrollRestore? {
        if (version <= scrollHandledVersion) return null
        scrollHandledVersion = version
        val restore = pendingScrollRestore
        pendingScrollRestore = null
        return restore
    }

    private fun syncBackStackDepth() {
        _state.update { it.copy(backStackDepth = history.size) }
    }

    private fun pushHistory(current: UiState) {
        if (history.size >= MAX_HISTORY) history.removeFirst()
        history.addLast(HistoryEntry(current.path, current.page, curScrollIndex, curScrollOffset))
        syncBackStackDepth()
    }

    init {
        _state.update { it.copy(serverSummary = appRef.serverSummary) }
        refreshReadingProgress()
        load("/")
    }

    // ------------------------------------------------------------ 继续阅读
    /**
     * 重新读一遍本地进度（书架卡片 + 列表进度条）。
     *
     * 由 init 与浏览页 ON_RESUME 调用：从阅读页返回时进度刚写过盘，
     * 不刷新的话卡片会停在"上次进来时"的页码。
     */
    fun refreshReadingProgress() {
        viewModelScope.launch {
            runCatchingIo {
                val all = appRef.progressStore.recent(RECENT_LOAD).first()
                // 只展示"当前服务器上确实存在该书库"的书：同一台设备可能连过多个
                // 服务器，而进度按 book_id 全局存，不筛会把 A 服务器的书摆到 B 的
                // 书架上（点开必然 404）。用书库名而不是服务器地址：局域网里 DHCP
                // 换 IP 是常事，按地址筛会让书架在换 IP 后整个消失。
                val roots = _state.value.libraryNames.toSet()
                val list = if (roots.isEmpty()) {
                    all
                } else {
                    all.filter { book ->
                        val root = book.progress.vpath.trimStart('/').substringBefore('/')
                        // 旧记录没有 vpath：无从判断，按可见处理
                        root.isEmpty() || root in roots
                    }
                }
                _state.update {
                    it.copy(
                        recent = list.take(RECENT_ON_SHELF),
                        progressById = list.mapNotNull { book ->
                            book.fraction?.let { fraction -> book.bookId to fraction }
                        }.toMap(),
                    )
                }
            }
        }
    }

    /** 打开"最近阅读"里的书（与点列表行同一条路径）。 */
    fun openRecent(bookId: String) {
        if (bookId.isEmpty()) {
            showMessage("这本书的进度记录缺少 book_id")
            return
        }
        _openBook.value = bookId
    }

    fun coverUrlForBook(bookId: String, width: Int = 240): String =
        repository.coverUrlForBook(bookId, width)

    // ------------------------------------------------------------ 书库搜索
    fun openSearch() {
        _state.update { it.copy(searchOpen = true) }
    }

    fun closeSearch() {
        searchJob?.cancel()
        // 保留查询与结果（再打开时接着看），只清掉上一次的错误
        _state.update { it.copy(searchOpen = false, searching = false, searchError = null) }
    }

    /**
     * 输入即搜（250ms 防抖）：书库搜索只查内存索引，单次几毫秒，
     * 但大书库每个按键都发一次请求没有意义，防抖把连打合并成一次。
     */
    fun onSearchQueryChange(value: String) {
        // 服务端 q 限长 120：超长粘贴在客户端截断，别让它变成一次 422
        val query = value.take(SEARCH_MAX_QUERY)
        _state.update { it.copy(searchQuery = query, searchError = null) }
        searchJob?.cancel()
        val keyword = query.trim()
        if (keyword.isEmpty()) {
            _state.update {
                it.copy(
                    searching = false,
                    searchResults = emptyList(),
                    searchTotal = 0,
                    searchTruncated = false,
                    searchScanning = false,
                    searchError = null,
                )
            }
            return
        }
        searchJob = viewModelScope.launch {
            delay(SEARCH_DEBOUNCE_MS)
            searchNow(keyword)
        }
    }

    /** 键盘"搜索"键：跳过防抖立即查。 */
    fun submitSearch() {
        val keyword = _state.value.searchQuery.trim()
        if (keyword.isEmpty()) return
        searchJob?.cancel()
        searchJob = viewModelScope.launch { searchNow(keyword) }
    }

    private suspend fun searchNow(keyword: String) {
        _state.update { it.copy(searching = true, searchError = null) }
        try {
            val response = repository.searchLibrary(keyword, SEARCH_LIMIT)
            // 过期响应防护：防抖任务被取消后仍可能已返回，关键词变了就不能覆盖
            if (_state.value.searchQuery.trim() != keyword) return
            _state.update {
                it.copy(
                    searching = false,
                    searchResults = response.entries,
                    searchTotal = response.total,
                    searchTruncated = response.truncated,
                    searchScanning = response.scanning,
                    searchError = null,
                )
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: ApiException) {
            if (_state.value.searchQuery.trim() != keyword) return
            // 错误留在面板里显示：Snackbar 会被底部面板盖住，用户只看到"没有匹配"
            _state.update { it.copy(searching = false, searchError = error.message) }
        }
    }

    /** 点搜索结果：能打开就直接进书（这是搜索的主要目的）。 */
    fun openSearchResult(entry: FileEntryDto) {
        val bookId = entry.bookId
        when {
            entry.isBook && !bookId.isNullOrEmpty() && entry.openable -> {
                closeSearch()
                _openBook.value = bookId
            }
            !entry.openable -> showMessage(entry.reason ?: "该格式暂不可用")
            else -> showMessage("这本书还没有索引，请稍后重试")
        }
    }

    /** 跳到搜索结果所在目录（书打不开 / 想看看同目录还有什么时用）。 */
    fun openContainingFolder(entry: FileEntryDto) {
        val path = entry.path.trimEnd('/')
        val parent = path.substringBeforeLast('/', "").ifEmpty { "/" }
        closeSearch()
        load(parent)
    }

    /**
     * 加载指定目录的指定页：取消上一次未完成的加载，避免旧响应覆盖新状态
     * （快速连点面包屑/文件夹/翻页）。page 默认 1 = 进入新目录；翻页时带当前 path 调用。
     * 切换目录时把离开处的（目录, 页码, 滚动位置）压入历史，供返回时恢复。
     */
    fun load(path: String, page: Int = 1) {
        loadJob?.cancel()
        val snapshot = _state.value
        val changingDir = path != snapshot.path
        if (changingDir) pushHistory(snapshot)
        startLoad(path, page, restore = null)
    }

    /**
     * 返回上一页（系统返回键 / 顶栏返回箭头）：弹出历史栈并恢复离开时的目录、
     * 页码与滚动位置；历史为空时退化为回到上一级目录。
     */
    fun navigateBack() {
        val entry = history.removeLastOrNull()
        if (entry == null) {
            navigateUp()
            return
        }
        syncBackStackDepth()
        loadJob?.cancel()
        val restore = ScrollRestore(entry.scrollIndex, entry.scrollOffset)
        pendingScrollRestore = restore
        startLoadInternal(entry.path, entry.page)
    }

    private fun startLoad(path: String, page: Int, restore: ScrollRestore?) {
        // 每次加载都覆盖待恢复位置：失败的旧恢复请求不能泄漏到下一次成功加载
        pendingScrollRestore = restore
        startLoadInternal(path, page)
    }

    private fun startLoadInternal(path: String, page: Int) {
        val changingDir = path != _state.value.path
        if (changingDir) {
            // 换目录期间旧列表已离屏、不再上报滚动位置：清零，避免把上一目录的
            // 位置错误快照进随后 push 的历史里（新列表组合后 snapshotFlow 会立即上报）
            curScrollIndex = 0
            curScrollOffset = 0
        }
        // 同目录翻页失败时列表仍是旧页内容：页码指示必须跟着回退（见 catch 分支），
        // 否则会出现"指示第 3 页、显示第 2 页"的错位
        val restorePage = if (changingDir) 1 else _state.value.page
        _state.update {
            it.copy(
                path = path,
                page = page,
                loading = true,
                error = null,
                // 切换目录时清空旧列表，避免加载中仍显示上一个目录的内容
                entries = if (changingDir) emptyList() else it.entries,
                gallery = if (changingDir) null else it.gallery,
            )
        }
        loadJob = viewModelScope.launch {
            try {
                var targetPage = page
                var response = repository.listFiles(path, PAGE_SIZE, (targetPage - 1) * PAGE_SIZE)
                if (_state.value.path != path) return@launch
                // 内容缩短后当前页可能已越过最后一页：自动收敛到最后一页重取
                if (response.entries.isEmpty() && targetPage > 1 && response.total > 0) {
                    targetPage = totalPagesOf(response.total)
                    response = repository.listFiles(path, PAGE_SIZE, (targetPage - 1) * PAGE_SIZE)
                    if (_state.value.path != path) return@launch
                }
                _state.update {
                    it.copy(
                        page = targetPage,
                        parent = response.parent,
                        gallery = response.gallery,
                        entries = response.entries,
                        total = response.total,
                        scanning = response.scanning,
                        loading = false,
                        serverSummary = appRef.serverSummary,
                        dataVersion = it.dataVersion + 1,
                        // 根目录响应才带书库清单；子目录响应没有就沿用上一次
                        libraryNames = response.libraries?.map { lib -> lib.name }
                            ?: it.libraryNames,
                    )
                }
                // 书库清单第一次到达：按它重新过滤"继续阅读"（初始化时可能还没拿到）
                if (response.libraries != null) refreshReadingProgress()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                if (_state.value.path != path || _state.value.page != page) return@launch
                if (_state.value.entries.isEmpty()) {
                    // 目录加载失败（列表为空）：整页错误视图
                    _state.update { it.copy(loading = false, error = error.message) }
                } else {
                    // 翻页失败：页码指示回退到已显示的旧页，保留当前列表，用 Snackbar 提示
                    _state.update {
                        it.copy(loading = false, page = restorePage, message = error.message ?: "加载失败")
                    }
                }
            }
        }
    }

    fun refresh() {
        loadJob?.cancel()
        // 不走 load()：路径不变不入历史，且保留未消费的待恢复位置——
        // "返回上一页"加载失败后点重试，成功时仍能恢复原页码与滚动位置
        startLoad(_state.value.path, _state.value.page, restore = pendingScrollRestore)
    }

    fun navigateUp() {
        // 加载中 parent 还是上一个目录的旧值：此时回上级会跳错目录
        // （如"返回书库根目录"加载途中连按返回），等加载完成后再走
        if (_state.value.loading) return
        val parent = _state.value.parent ?: return
        load(parent)
    }

    /** 跳到指定页（越界自动收敛并提示）；已在目标页时不重复请求。 */
    fun goToPage(page: Int) {
        val snapshot = _state.value
        if (snapshot.loading) return
        val target = page.coerceIn(1, snapshot.totalPages)
        if (page != target) showMessage("页码超出范围（1 - ${snapshot.totalPages}）")
        if (target == snapshot.page) return
        load(snapshot.path, target)
    }

    fun firstPage() = goToPage(1)

    fun lastPage() = goToPage(_state.value.totalPages)

    fun nextPage() = goToPage(_state.value.page + 1)

    fun prevPage() = goToPage(_state.value.page - 1)

    fun onEntryClick(entry: FileEntryDto) {
        when {
            entry.isDir -> load(entry.path)
            entry.isBook -> {
                val bookId = entry.bookId
                if (bookId.isNullOrEmpty()) {
                    showMessage("该文件还未建立索引，请稍候重试")
                } else if (!entry.openable) {
                    showMessage(entry.reason ?: "该格式暂不可用")
                } else {
                    _openBook.value = bookId
                }
            }
            entry.isImageFile -> {
                // 服务器可能在 gallery.min_images_in_folder > 1 时把单张图片标成不可打开
                // （列表已给出 reason，界面上"打开为图集"按钮也隐藏了）。这里必须同样尊重它，
                // 否则点一行会先发一次注定 415 的请求，再把服务端的原始错误弹给用户。
                if (entry.openable && entry.gallery?.canOpenAsBook == true) {
                    openFolderAsGallery(entry.path)
                } else {
                    showMessage(entry.reason ?: "该图片无法作为图集打开")
                }
            }
            entry.isMedia -> {
                if (!entry.openable) {
                    showMessage(entry.reason ?: "该媒体无法播放")
                } else {
                    val url = repository.mediaUrl(entry)
                    if (url.isNullOrEmpty()) {
                        showMessage("无法获取媒体地址")
                    } else {
                        _openMedia.value = MediaTarget(
                            url = url,
                            title = entry.name,
                            audioOnly = entry.isAudio,
                        )
                    }
                }
            }
            else -> showMessage(entry.reason ?: "无法打开该文件")
        }
    }

    /** 把目录（或单张图片）注册为虚拟图集书并打开（设计 7.5）。 */
    fun openFolderAsGallery(path: String) {
        if (_state.value.opening) return
        // 同步置位：放在 launch 里面的话，同一帧内的连点会通过检查并打开两次
        _state.update { it.copy(opening = true) }
        viewModelScope.launch {
            try {
                val response = repository.openFolder(path)
                _state.update { it.copy(opening = false) }
                _openBook.value = response.bookId
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                _state.update { it.copy(opening = false, message = error.message) }
            }
        }
    }

    fun consumeOpenBook() {
        _openBook.value = null
    }

    fun consumeOpenMedia() {
        _openMedia.value = null
    }

    /** 书封 URL（Composable 不直接触碰 repository，设计分层；带服务端 rev）。 */
    fun coverUrl(entry: FileEntryDto, width: Int = 240): String? =
        repository.coverUrl(entry, width)

    fun consumeMessage() {
        _state.update { it.copy(message = null) }
    }

    fun showMessage(text: String) {
        _state.update { it.copy(message = text) }
    }

    private companion object {
        /** 每页条目数（服务器 /files 的 limit 上限为 5000；页大小取小些便于按页翻找）。 */
        const val PAGE_SIZE = 100

        /** 目录历史栈深度上限（防无限深度浏览撑爆内存）。 */
        const val MAX_HISTORY = 32

        /** "继续阅读"最多显示几本；读多少条用来算列表进度条。 */
        const val RECENT_ON_SHELF = 8
        const val RECENT_LOAD = 50

        /** 书库搜索：结果条数上限与输入防抖。 */
        const val SEARCH_LIMIT = 50
        const val SEARCH_DEBOUNCE_MS = 250L

        /** 与服务端 `q` 的 max_length 一致（避免超长输入换来一次 422）。 */
        const val SEARCH_MAX_QUERY = 120

        fun totalPagesOf(total: Int): Int = if (total <= 0) 1 else (total + PAGE_SIZE - 1) / PAGE_SIZE
    }
}
