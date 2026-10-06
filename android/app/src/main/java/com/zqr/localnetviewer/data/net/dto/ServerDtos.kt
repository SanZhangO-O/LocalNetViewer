package com.zqr.localnetviewer.data.net.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/** 服务信息 `GET /api/v1/server/info`。 */
@Serializable
data class ServerInfoDto(
    val name: String = "LocalNetViewer",
    val version: String = "",
    @SerialName("api_version") val apiVersion: String = "v1",
    @SerialName("auth_required") val authRequired: Boolean = false,
    val libraries: List<LibraryInfoDto> = emptyList(),
    val features: FeaturesDto = FeaturesDto(),
    @SerialName("server_time") val serverTime: String = "",
)

@Serializable
data class LibraryInfoDto(
    val name: String,
    @SerialName("book_count") val bookCount: Int = 0,
    val path: String? = null,
)

@Serializable
data class FeaturesDto(
    val png: Boolean = true,
    val json: Boolean = true,
    val search: Boolean = true,
    val gallery: Boolean = true,
    val fits: List<String> = listOf("viewport", "original"),
    val strip: Boolean = true,
    @SerialName("progress_sync") val progressSync: Boolean = false,
    /** 压缩包内容浏览（设计 7.6）；旧服务器缺省 false，客户端隐藏"查看内容"入口。 */
    @SerialName("archive_browse") val archiveBrowse: Boolean = false,
    @SerialName("strip_min_aspect") val stripMinAspect: Float = 1.5f,
)

/** 统一错误模型（设计 5.1）。 */
@Serializable
data class ErrorDto(
    val code: String = "INTERNAL",
    val message: String = "",
    val detail: Map<String, String> = emptyMap(),
)
