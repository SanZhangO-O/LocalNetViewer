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
 * 目录优先 + 自然排序由服务器完成；这里负责分页加载、"打开为图集"入口与错误提示。
 */
class BrowserViewModel(app: App) : AndroidViewModel(app) {

    private val repository = app.libraryRepository

    data class UiState(
        val path: String = "/",
        val parent: String? = null,
        val gallery: GalleryInfoDto? = null,
        val entries: List<FileEntryDto> = emptyList(),
        val total: Int = 0,
        val loading: Boolean = true,
        val loadingMore: Boolean = false,
        val opening: Boolean = false,
        val scanning: Boolean = false,
        val error: String? = null,
        val message: String? = null,
    ) {
        val canOpenCurrentAsGallery: Boolean
            get() = gallery?.canOpenAsBook == true

        val currentName: String
            get() = if (path == "/") "书库" else path.trimEnd('/').substringAfterLast('/')

        val hasMore: Boolean get() = entries.size < total
    }

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** 需要打开的书（点书籍、或把文件夹注册为图集后）。 */
    private val _openBook = MutableStateFlow<String?>(null)
    val openBook: StateFlow<String?> = _openBook.asStateFlow()

    private var loadJob: Job? = null
    private var loadMoreJob: Job? = null

    init {
        load("/")
    }

    /** 加载目录：取消上一次未完成的加载，避免旧响应覆盖新目录（快速连点面包屑/文件夹）。 */
    fun load(path: String) {
        loadJob?.cancel()
        loadMoreJob?.cancel()
        loadJob = viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null, loadingMore = false) }
            try {
                val response = repository.listFiles(path, PAGE_SIZE, 0)
                _state.update {
                    it.copy(
                        path = response.path,
                        parent = response.parent,
                        gallery = response.gallery,
                        entries = response.entries,
                        total = response.total,
                        scanning = response.scanning,
                        loading = false,
                        loadingMore = false,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                _state.update { it.copy(loading = false, error = error.message) }
            }
        }
    }

    fun refresh() {
        load(_state.value.path)
    }

    fun navigateUp() {
        val parent = _state.value.parent ?: return
        load(parent)
    }

    fun loadMore() {
        val snapshot = _state.value
        if (snapshot.loadingMore || !snapshot.hasMore) return
        val requestedPath = snapshot.path
        _state.update { it.copy(loadingMore = true) }
        loadMoreJob?.cancel()
        loadMoreJob = viewModelScope.launch {
            try {
                val response = repository.listFiles(requestedPath, PAGE_SIZE, snapshot.entries.size)
                // 目录可能已经切换：丢弃过期响应的追加结果
                if (_state.value.path != requestedPath) return@launch
                _state.update {
                    it.copy(
                        entries = it.entries + response.entries,
                        total = response.total,
                        scanning = response.scanning,
                        loadingMore = false,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                _state.update { it.copy(loadingMore = false, message = error.message) }
            }
        }
    }

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

    fun consumeMessage() {
        _state.update { it.copy(message = null) }
    }

    fun showMessage(text: String) {
        _state.update { it.copy(message = text) }
    }

    private companion object {
        const val PAGE_SIZE = 300
    }
}
