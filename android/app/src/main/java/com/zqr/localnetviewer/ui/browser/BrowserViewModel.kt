package com.zqr.localnetviewer.ui.browser

import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.ApiException
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.data.net.dto.GalleryInfoDto
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
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

    private var loadJob: Job? = null

    init {
        _state.update { it.copy(serverSummary = appRef.serverSummary) }
        load("/")
    }

    /**
     * 加载指定目录的指定页：取消上一次未完成的加载，避免旧响应覆盖新状态
     * （快速连点面包屑/文件夹/翻页）。page 默认 1 = 进入新目录；翻页时带当前 path 调用。
     */
    fun load(path: String, page: Int = 1) {
        loadJob?.cancel()
        val changingDir = path != _state.value.path
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
                    )
                }
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
        load(_state.value.path, _state.value.page)
    }

    fun navigateUp() {
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

    /** 书封 URL（Composable 不直接触碰 repository，设计分层；带服务端 rev）。 */
    fun coverUrl(entry: FileEntryDto, width: Int = 240): String =
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

        fun totalPagesOf(total: Int): Int = if (total <= 0) 1 else (total + PAGE_SIZE - 1) / PAGE_SIZE
    }
}
