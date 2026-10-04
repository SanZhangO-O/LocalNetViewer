package com.zqr.localnetviewer.data.net.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * 排版参数 `POST /api/v1/books/{id}/layout`（设计 4.6）。
 *
 * 所有尺寸都是**物理像素**：客户端负责把 sp/dp 按 density 换算后再上报。
 */
@Serializable
data class LayoutProfileDto(
    val viewport: ViewportDto,
    val margin: MarginDto = MarginDto(),
    val font: FontSpecDto = FontSpecDto(),
    val paragraph: ParagraphSpecDto = ParagraphSpecDto(),
    val image: ImageSpecDto = ImageSpecDto(),
    @SerialName("chapter_new_page") val chapterNewPage: Boolean = true,
    /** flow（文本重排）/ fixed（PDF、漫画单页）/ strip（条漫切片） */
    val mode: String = "flow",
    @SerialName("strip_overlap_px") val stripOverlapPx: Int = 0,
)

@Serializable
data class ViewportDto(val w: Int, val h: Int)

@Serializable
data class MarginDto(
    val left: Int = 0,
    val right: Int = 0,
    val top: Int = 0,
    val bottom: Int = 0,
)

@Serializable
data class FontSpecDto(
    val family: String = "auto",
    @SerialName("size_px") val sizePx: Int = 52,
    @SerialName("line_height") val lineHeight: Float = 1.6f,
    @SerialName("letter_spacing_px") val letterSpacingPx: Float = 0f,
)

@Serializable
data class ParagraphSpecDto(
    @SerialName("spacing_px") val spacingPx: Float = 20f,
    @SerialName("first_line_indent_chars") val firstLineIndentChars: Int = 2,
)

@Serializable
data class ImageSpecDto(
    @SerialName("max_width_ratio") val maxWidthRatio: Float = 1f,
    @SerialName("max_height_ratio") val maxHeightRatio: Float = 1f,
)

@Serializable
data class LayoutResponseDto(
    @SerialName("layout_id") val layoutId: String = "",
    val mode: String = "flow",
    @SerialName("total_pages") val totalPages: Int = 0,
    val chapters: List<ChapterInfoDto> = emptyList(),
    @SerialName("cache_hit") val cacheHit: Boolean = false,
    val strip: Boolean = false,
    /** 内容修订号：同 URL 内容变化时递增，页面 URL 携带它避免命中旧磁盘缓存（旧服务器缺省为空串）。 */
    val revision: String = "",
)

/** 锚点：所有阅读位置都用它表示，而不是页码（设计 2.3）。 */
@Serializable
data class AnchorDto(
    val chapter: Int = 0,
    val block: Int = 0,
    val char: Int = 0,
)

@Serializable
data class PageMetaDto(
    val page: Int = 1,
    @SerialName("total_pages") val totalPages: Int = 0,
    val anchor: AnchorDto = AnchorDto(),
    @SerialName("chapter_index") val chapterIndex: Int = 0,
    @SerialName("chapter_title") val chapterTitle: String = "",
    @SerialName("first_line") val firstLine: String = "",
    val mode: String = "flow",
    val aspect: Float? = null,
)

@Serializable
data class LocateResponseDto(
    val page: Int = 1,
    @SerialName("total_pages") val totalPages: Int = 0,
    val ratio: Float = 0f,
    @SerialName("chapter_index") val chapterIndex: Int = 0,
    @SerialName("chapter_title") val chapterTitle: String = "",
    val clamped: Boolean = false,
)

@Serializable
data class SearchResponseDto(
    val query: String = "",
    val total: Int = 0,
    val results: List<SearchResultDto> = emptyList(),
    val limited: Boolean = false,
)

@Serializable
data class SearchResultDto(
    @SerialName("chapter_index") val chapterIndex: Int = 0,
    @SerialName("chapter_title") val chapterTitle: String = "",
    val block: Int = 0,
    val char: Int = 0,
    val page: Int? = null,
    val snippet: String = "",
)

/** 字体列表 `GET /api/v1/render/fonts`。 */
@Serializable
data class FontsResponseDto(
    val default: String = "",
    val fonts: List<FontInfoDto> = emptyList(),
)

@Serializable
data class FontInfoDto(
    val id: String = "",
    val name: String = "",
    @SerialName("file_name") val fileName: String = "",
    val default: Boolean = false,
    val builtin: Boolean = false,
)

/** 服务器端进度同步（P4 可选）。 */
@Serializable
data class ProgressDto(
    @SerialName("book_id") val bookId: String = "",
    @SerialName("device_id") val deviceId: String? = null,
    val anchor: AnchorDto = AnchorDto(),
    val page: Int? = null,
    val ratio: Float? = null,
    @SerialName("updated_at") val updatedAt: String = "",
)

@Serializable
data class ProgressPutDto(
    val anchor: AnchorDto,
    val page: Int? = null,
    val ratio: Float? = null,
)
