package com.zqr.localnetviewer.data.repo

import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.net.apiCall
import com.zqr.localnetviewer.data.net.dto.ArchiveContentsResponseDto
import com.zqr.localnetviewer.data.net.dto.ArchiveEntryItemDto
import com.zqr.localnetviewer.data.net.dto.BookMetaDto
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.data.net.dto.FilesResponseDto
import com.zqr.localnetviewer.data.net.dto.OpenArchiveRequestDto
import com.zqr.localnetviewer.data.net.dto.OpenArchiveResponseDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderRequestDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderResponseDto
import com.zqr.localnetviewer.data.net.dto.ServerInfoDto

/** 书库相关：连接、目录浏览、元数据、图集注册、压缩包内容浏览（设计 6.2 / 7.6）。 */
class LibraryRepository(private val client: ApiClient) {

    suspend fun serverInfo(): ServerInfoDto = apiCall(client) { client.api().serverInfo() }

    suspend fun listFiles(path: String, limit: Int = 500, offset: Int = 0): FilesResponseDto =
        apiCall(client) { client.api().listFiles(path, limit, offset) }

    suspend fun bookMeta(bookId: String): BookMetaDto =
        apiCall(client) { client.api().bookMeta(bookId) }

    /** 把文件夹（或单张图片）注册为虚拟图集书（设计 7.5）。 */
    suspend fun openFolder(path: String, recursive: Boolean? = null): OpenFolderResponseDto =
        apiCall(client) { client.api().openFolder(OpenFolderRequestDto(path, recursive)) }

    /** 压缩包内部目录列表（设计 7.6）。 */
    suspend fun archiveContents(
        bookId: String,
        entry: String = "",
        limit: Int = 200,
        offset: Int = 0,
    ): ArchiveContentsResponseDto =
        apiCall(client) { client.api().bookContents(bookId, entry, limit, offset) }

    /** 包内条目 → 嵌套书（幂等，设计 7.6）。 */
    suspend fun openArchiveEntry(bookId: String, entry: String): OpenArchiveResponseDto =
        apiCall(client) { client.api().openArchiveEntry(bookId, OpenArchiveRequestDto(entry)) }

    /** 压缩包内图片条目的预览 URL（服务端下发相对路径，带 rev）。 */
    fun archivePreviewUrl(item: ArchiveEntryItemDto): String? {
        val provided = item.previewUrl?.takeIf { it.isNotBlank() } ?: return null
        return client.absoluteUrl(provided)
    }

    /** 音频/视频流地址（服务端下发的 stream_url，支持 Range，M5）。 */
    fun mediaUrl(entry: FileEntryDto): String? {
        val provided = entry.streamUrl?.takeIf { it.isNotBlank() } ?: return null
        return client.absoluteUrl(provided)
    }

    /**
     * 服务端下发的 `cover_url`（带 `rev`，内容变化后 URL 变化、缓存自然失效）。
     *
     * 服务端 cover_url 只带 rev：本方法承诺的 width 必须补上，
     * 否则调用方会静默拿到服务端默认尺寸。
     */
    fun coverUrl(entry: FileEntryDto, width: Int = 240): String? {
        val provided = entry.coverUrl?.takeIf { it.isNotBlank() } ?: return null
        val withWidth = if ('?' in provided) "$provided&w=$width" else "$provided?w=$width"
        return client.absoluteUrl(withWidth)
    }
}
