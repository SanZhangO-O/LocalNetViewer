package com.zqr.localnetviewer.data.repo

import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.net.ApiPaths
import com.zqr.localnetviewer.data.net.apiCall
import com.zqr.localnetviewer.data.net.dto.BookMetaDto
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.data.net.dto.FilesResponseDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderRequestDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderResponseDto
import com.zqr.localnetviewer.data.net.dto.ServerInfoDto

/** 书库相关：连接、目录浏览、元数据、图集注册（设计 6.2）。 */
class LibraryRepository(private val client: ApiClient) {

    suspend fun serverInfo(): ServerInfoDto = apiCall(client) { client.api().serverInfo() }

    suspend fun listFiles(path: String, limit: Int = 500, offset: Int = 0): FilesResponseDto =
        apiCall(client) { client.api().listFiles(path, limit, offset) }

    suspend fun bookMeta(bookId: String): BookMetaDto =
        apiCall(client) { client.api().bookMeta(bookId) }

    /** 把文件夹（或单张图片）注册为虚拟图集书（设计 7.5）。 */
    suspend fun openFolder(path: String, recursive: Boolean? = null): OpenFolderResponseDto =
        apiCall(client) { client.api().openFolder(OpenFolderRequestDto(path, recursive)) }

    fun coverUrl(bookId: String, width: Int = 240): String =
        client.absoluteUrl(ApiPaths.cover(bookId, width))

    /**
     * 优先使用服务端下发的 `cover_url`（带 `rev`，内容变化后 URL 变化、缓存自然失效）；
     * 旧服务器缺字段时回退到本地拼 URL。
     */
    fun coverUrl(entry: FileEntryDto, width: Int = 240): String {
        val provided = entry.coverUrl?.takeIf { it.isNotBlank() }
        if (provided != null) {
            // 服务端 cover_url 只带 rev：本方法承诺的 width 必须补上，
            // 否则调用方会静默拿到服务端默认尺寸，两个构造器就此漂移。
            val withWidth = if ('?' in provided) "$provided&w=$width" else "$provided?w=$width"
            return client.absoluteUrl(withWidth)
        }
        return coverUrl(entry.bookId ?: "", width)
    }
}
