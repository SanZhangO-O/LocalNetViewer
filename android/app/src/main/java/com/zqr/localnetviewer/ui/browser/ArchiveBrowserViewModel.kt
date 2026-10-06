package com.zqr.localnetviewer.ui.browser

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.ApiException
import com.zqr.localnetviewer.data.net.dto.ArchiveEntryItemDto
import com.zqr.localnetviewer.data.net.dto.ArchiveSummaryDto
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * 压缩包内容浏览状态机（设计 7.6 ArchiveBrowserScreen）。
 *
 * 与文件浏览页同风格：包内目录导航 + 分页累加加载；条目点按通过
 * contents/open 注册为嵌套书后交给阅读器（bookId 与普通书一致）。
 */
class ArchiveBrowserViewModel(
    app: App,
    /** 压缩包（或嵌套压缩包）书籍的 bookId。 */
    val bookId: String,
) : ViewModel() {

    private val repository = app.libraryRepository

    data class UiState(
        val entry: String = "",
        /** 包内父目录；包根为 null（此时返回键退出整页）。 */
        val parent: String? = null,
        val entries: List<ArchiveEntryItemDto> = emptyList(),
        val total: Int = 0,
        val summary: ArchiveSummaryDto = ArchiveSummaryDto(),
        val loading: Boolean = true,
        val openingPath: String? = null,
        val error: String? = null,
        val message: String? = null,
    ) {
        val canNavigateUp: Boolean get() = parent != null
        val hasMore: Boolean get() = entries.size < total
    }

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** 打开成功的嵌套书 bookId（UI 领取后导航到阅读器）。 */
    private val _openBook = MutableStateFlow<String?>(null)
    val openBook: StateFlow<String?> = _openBook.asStateFlow()

    private var loadJob: Job? = null
    private var openJob: Job? = null

    init {
        load("")
    }

    /** 进入包内目录（空串 = 包根）；目录栈由服务器 parent 驱动，这里只发请求。 */
    fun load(entry: String) {
        loadJob?.cancel()
        _state.update {
            it.copy(
                entry = entry,
                loading = true,
                error = null,
                entries = emptyList(),
                total = 0,
            )
        }
        loadJob = viewModelScope.launch {
            try {
                val response = repository.archiveContents(bookId, entry, PAGE_SIZE, 0)
                if (_state.value.entry != entry) return@launch
                _state.update {
                    it.copy(
                        parent = response.parent,
                        entries = response.entries,
                        total = response.total,
                        summary = response.archive,
                        loading = false,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                if (_state.value.entry != entry) return@launch
                _state.update { it.copy(loading = false, error = error.message ?: "加载失败") }
            }
        }
    }

    /** 累加下一页（列表底部"加载更多"）。 */
    fun loadMore() {
        val snapshot = _state.value
        if (snapshot.loading || !snapshot.hasMore) return
        viewModelScope.launch {
            try {
                val response = repository.archiveContents(
                    bookId, snapshot.entry, PAGE_SIZE, snapshot.entries.size
                )
                if (_state.value.entry != snapshot.entry) return@launch
                _state.update { current ->
                    current.copy(entries = (current.entries + response.entries).distinctBy { it.path })
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                _state.update { it.copy(message = error.message ?: "加载失败") }
            }
        }
    }

    fun navigateUp() {
        val parent = _state.value.parent
        if (parent != null) load(parent)
    }

    fun refresh() = load(_state.value.entry)

    /** 点按条目：目录下钻；书籍/图片注册为嵌套书并打开；其它文件提示原因。 */
    fun onEntryClick(item: ArchiveEntryItemDto) {
        when {
            item.isDir -> load(item.path)
            item.isBook || item.isImage -> openEntry(item)
            else -> showMessage(item.reason ?: "无法打开该文件")
        }
    }

    /** 目录作为图集打开（服务器对目录条目做同样的 contents/open）。 */
    fun openAsGallery(item: ArchiveEntryItemDto) {
        if (!item.isDir) return
        openEntry(item)
    }

    private fun openEntry(item: ArchiveEntryItemDto) {
        if (openJob?.isActive == true) return
        if (!item.openable) {
            showMessage(item.reason ?: "该条目暂不可打开")
            return
        }
        _state.update { it.copy(openingPath = item.path, message = null) }
        openJob = viewModelScope.launch {
            try {
                val response = repository.openArchiveEntry(bookId, item.path)
                _state.update { it.copy(openingPath = null) }
                _openBook.value = response.bookId
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                _state.update {
                    it.copy(
                        openingPath = null,
                        message = when {
                            error.isNotFound -> "条目不存在或压缩包已变化，请刷新重试"
                            else -> error.message ?: "打开失败"
                        },
                    )
                }
            }
        }
    }

    fun consumeOpenBook() {
        _openBook.value = null
    }

    /** 图片条目预览 URL（服务端下发相对路径 + rev；无则返回 null）。 */
    fun previewUrl(item: ArchiveEntryItemDto): String? = repository.archivePreviewUrl(item)

    fun consumeMessage() {
        _state.update { it.copy(message = null) }
    }

    private fun showMessage(text: String) {
        _state.update { it.copy(message = text) }
    }

    companion object {
        /** 每页条目数（服务器 contents 的 limit 上限为 2000）。 */
        const val PAGE_SIZE = 200
    }
}

/** /files 列表里压缩包格式的书籍条目（"查看内容"入口判定，设计 7.6）。 */
val ARCHIVE_BOOK_FORMATS = setOf("zip", "cbz", "rar", "cbr", "7z", "cb7", "tar", "cbt")

fun com.zqr.localnetviewer.data.net.dto.FileEntryDto.isArchiveBook(): Boolean =
    isBook && format?.lowercase() in ARCHIVE_BOOK_FORMATS
