package com.zqr.localnetviewer.data.net

import com.zqr.localnetviewer.data.net.dto.BookMetaDto
import com.zqr.localnetviewer.data.net.dto.ArchiveContentsResponseDto
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.data.net.dto.FilesResponseDto
import com.zqr.localnetviewer.data.net.dto.FontsResponseDto
import com.zqr.localnetviewer.data.net.dto.LayoutProfileDto
import com.zqr.localnetviewer.data.net.dto.LayoutResponseDto
import com.zqr.localnetviewer.data.net.dto.LocateResponseDto
import com.zqr.localnetviewer.data.net.dto.OpenArchiveRequestDto
import com.zqr.localnetviewer.data.net.dto.OpenArchiveResponseDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderRequestDto
import com.zqr.localnetviewer.data.net.dto.OpenFolderResponseDto
import com.zqr.localnetviewer.data.net.dto.PageMetaDto
import com.zqr.localnetviewer.data.net.dto.PageJsonDto
import com.zqr.localnetviewer.data.net.dto.ProgressDto
import com.zqr.localnetviewer.data.net.dto.ProgressPutDto
import com.zqr.localnetviewer.data.net.dto.SearchResponseDto
import com.zqr.localnetviewer.data.net.dto.ServerInfoDto
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query

/** 端点定义（设计 5.2 总表）。 */
interface ApiService {

    @GET("api/v1/server/info")
    suspend fun serverInfo(): ServerInfoDto

    @GET("api/v1/files")
    suspend fun listFiles(
        @Query("path") path: String = "/",
        @Query("limit") limit: Int = 1000,
        @Query("offset") offset: Int = 0,
    ): FilesResponseDto

    @GET("api/v1/books/{bookId}/meta")
    suspend fun bookMeta(@Path("bookId") bookId: String): BookMetaDto

    @POST("api/v1/books/{bookId}/layout")
    suspend fun registerLayout(
        @Path("bookId") bookId: String,
        @Body profile: LayoutProfileDto,
    ): LayoutResponseDto

    @GET("api/v1/books/{bookId}/pages/{page}/meta")
    suspend fun pageMeta(
        @Path("bookId") bookId: String,
        @Path("page") page: Int,
        @Query("layout_id") layoutId: String,
        @Query("rev") rev: String? = null,
    ): PageMetaDto

    /**
     * JSON 模式页面（文字直发）：服务器按行下发已排好的内容。
     *
     * 仅 flow 文本书支持；固定版式（PDF/漫画）服务器会返回 400，客户端不应请求。
     */
    @GET("api/v1/books/{bookId}/pages/{page}")
    suspend fun flowPage(
        @Path("bookId") bookId: String,
        @Path("page") page: Int,
        @Query("layout_id") layoutId: String,
        @Query("format") format: String = "json",
    ): PageJsonDto

    @GET("api/v1/books/{bookId}/locate")
    suspend fun locate(
        @Path("bookId") bookId: String,
        @Query("layout_id") layoutId: String,
        @Query("chapter") chapter: Int,
        @Query("block") block: Int,
        @Query("char") char: Int,
    ): LocateResponseDto

    @GET("api/v1/books/{bookId}/search")
    suspend fun search(
        @Path("bookId") bookId: String,
        @Query("q") query: String,
        @Query("layout_id") layoutId: String? = null,
        @Query("limit") limit: Int = 50,
    ): SearchResponseDto

    @POST("api/v1/books/open-folder")
    suspend fun openFolder(@Body body: OpenFolderRequestDto): OpenFolderResponseDto

    /** 压缩包内部目录列表（设计 7.6）。 */
    @GET("api/v1/books/{bookId}/contents")
    suspend fun bookContents(
        @Path("bookId") bookId: String,
        @Query("entry") entry: String = "",
        @Query("limit") limit: Int = 200,
        @Query("offset") offset: Int = 0,
    ): ArchiveContentsResponseDto

    /** 包内条目 → 嵌套书（幂等）。 */
    @POST("api/v1/books/{bookId}/contents/open")
    suspend fun openArchiveEntry(
        @Path("bookId") bookId: String,
        @Body body: OpenArchiveRequestDto,
    ): OpenArchiveResponseDto

    @GET("api/v1/render/fonts")
    suspend fun fonts(): FontsResponseDto

    @GET("api/v1/progress/{bookId}")
    suspend fun getProgress(
        @Path("bookId") bookId: String,
        @Query("device_id") deviceId: String? = null,
    ): ProgressDto

    @PUT("api/v1/progress/{bookId}")
    suspend fun putProgress(
        @Path("bookId") bookId: String,
        @Query("device_id") deviceId: String? = null,
        @Body body: ProgressPutDto,
    ): ProgressDto
}

/** URL 构造：页面图片/封面/书内资源都直接由 Coil 按 URL 拉取，不走 Retrofit。 */
object ApiPaths {

    /**
     * 页面图片 URL（设计 5.3）。
     *
     * `layout_id` 变化即 URL 变化，天然失效；ETag + immutable 由服务器下发。
     */
    fun page(
        bookId: String,
        page: Int,
        layoutId: String,
        theme: String = "white",
        scale: Float = 1f,
        fit: String = "viewport",
        quality: Int = 85,
        pair: Boolean = false,
        rtl: Boolean = false,
        format: String = "png",
        fixed: Boolean = false,
        revision: String = "",
    ): String {
        val builder = StringBuilder("/api/v1/books/$bookId/pages/$page")
        builder.append("?layout_id=").append(encode(layoutId))
        builder.append("&format=").append(encode(format))
        if (fixed) {
            builder.append("&fit=").append(encode(fit))
            builder.append("&quality=").append(quality)
            if (pair) builder.append("&pair=1")
            if (rtl) builder.append("&rtl=1")
        } else {
            builder.append("&theme=").append(encode(theme))
        }
        builder.append("&scale=").append(formatScale(scale))
        // 内容修订号变化 = 同 URL 内容变化：带上它让 Coil 的磁盘缓存无法命中旧页面
        if (revision.isNotEmpty()) {
            builder.append("&rev=").append(encode(revision))
        }
        return builder.toString()
    }

    fun asset(bookId: String, assetPath: String): String =
        "/api/v1/books/$bookId/asset?path=${encode(assetPath)}"

    private fun encode(value: String): String =
        java.net.URLEncoder.encode(value, "UTF-8")

    private fun formatScale(scale: Float): String {
        val clamped = scale.coerceIn(0.5f, 2.0f)
        return if (clamped == clamped.toInt().toFloat()) {
            "${clamped.toInt()}.0"
        } else {
            String.format(java.util.Locale.US, "%.2f", clamped)
        }
    }
}

/** 目录条目扩展：是否提供"打开为图集"入口（设计 7.5）。 */
val FileEntryDto.canOpenAsGallery: Boolean
    get() = (isDir || isImageFile) && (gallery?.canOpenAsBook == true)
