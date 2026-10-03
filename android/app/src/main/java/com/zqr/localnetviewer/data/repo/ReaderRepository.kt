package com.zqr.localnetviewer.data.repo

import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.net.ApiPaths
import com.zqr.localnetviewer.data.net.apiCall
import com.zqr.localnetviewer.data.net.dto.AnchorDto
import com.zqr.localnetviewer.data.net.dto.FontsResponseDto
import com.zqr.localnetviewer.data.net.dto.LayoutProfileDto
import com.zqr.localnetviewer.data.net.dto.LayoutResponseDto
import com.zqr.localnetviewer.data.net.dto.LocateResponseDto
import com.zqr.localnetviewer.data.net.dto.PageMetaDto
import com.zqr.localnetviewer.data.net.dto.ProgressDto
import com.zqr.localnetviewer.data.net.dto.ProgressPutDto
import com.zqr.localnetviewer.data.net.dto.SearchResponseDto

/** 阅读相关：排版注册、页元数据、锚点定位、搜索、页面 URL（设计 6.2）。 */
class ReaderRepository(private val client: ApiClient) {

    suspend fun registerLayout(bookId: String, profile: LayoutProfileDto): LayoutResponseDto =
        apiCall(client) { client.api().registerLayout(bookId, profile) }

    suspend fun pageMeta(bookId: String, page: Int, layoutId: String): PageMetaDto =
        apiCall(client) { client.api().pageMeta(bookId, page, layoutId) }

    suspend fun locate(bookId: String, layoutId: String, anchor: AnchorDto): LocateResponseDto =
        apiCall(client) {
            client.api().locate(bookId, layoutId, anchor.chapter, anchor.block, anchor.char)
        }

    suspend fun search(bookId: String, query: String, layoutId: String?): SearchResponseDto =
        apiCall(client) { client.api().search(bookId, query, layoutId) }

    suspend fun fonts(): FontsResponseDto = apiCall(client) { client.api().fonts() }

    suspend fun putProgress(
        bookId: String,
        anchor: AnchorDto,
        page: Int,
        ratio: Float,
        deviceId: String? = null,
    ): ProgressDto = apiCall(client) {
        client.api().putProgress(bookId, deviceId, ProgressPutDto(anchor, page, ratio))
    }

    suspend fun getProgress(bookId: String, deviceId: String? = null): ProgressDto =
        apiCall(client) { client.api().getProgress(bookId, deviceId) }

    /** 页面图片 URL：layout_id / theme / scale / fit 变化都会产生新 URL（天然失效）。 */
    fun pageUrl(
        bookId: String,
        page: Int,
        layoutId: String,
        theme: String,
        scale: Float,
        fit: String,
        quality: Int,
        pair: Boolean,
        rtl: Boolean,
        fixed: Boolean,
    ): String = client.absoluteUrl(
        ApiPaths.page(
            bookId = bookId,
            page = page,
            layoutId = layoutId,
            theme = theme,
            scale = scale,
            fit = fit,
            quality = quality,
            pair = pair,
            rtl = rtl,
            fixed = fixed,
        )
    )
}
