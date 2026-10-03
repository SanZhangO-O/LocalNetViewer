package com.zqr.localnetviewer.data.repo

import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.net.ApiPaths
import com.zqr.localnetviewer.data.net.apiCall
import com.zqr.localnetviewer.data.net.dto.BookMetaDto
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
}
