package com.zqr.localnetviewer.data.net.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/** 目录列表 `GET /api/v1/files`。 */
@Serializable
data class FilesResponseDto(
    val path: String = "/",
    val parent: String? = null,
    val gallery: GalleryInfoDto? = null,
    val entries: List<FileEntryDto> = emptyList(),
    val total: Int = 0,
    val limit: Int = 1000,
    val offset: Int = 0,
    val scanning: Boolean = false,
    val libraries: List<LibraryInfoDto>? = null,
)

@Serializable
data class GalleryInfoDto(
    @SerialName("image_count") val imageCount: Int = 0,
    @SerialName("can_open_as_book") val canOpenAsBook: Boolean = false,
)

/** 书库搜索 `GET /api/v1/library/search`（只匹配索引里的书名/路径）。 */
@Serializable
data class LibrarySearchResponseDto(
    val query: String = "",
    val entries: List<FileEntryDto> = emptyList(),
    val total: Int = 0,
    /** 结果被 limit 截断（total > entries.size）。 */
    val truncated: Boolean = false,
    val scanning: Boolean = false,
)

@Serializable
data class FileEntryDto(
    val name: String,
    /** dir | book | file | image | media */
    val type: String = "file",
    val path: String = "",
    val format: String? = null,
    @SerialName("book_id") val bookId: String? = null,
    val size: Long? = null,
    val mtime: String? = null,
    @SerialName("cover_url") val coverUrl: String? = null,
    val gallery: GalleryInfoDto? = null,
    val openable: Boolean = true,
    val reason: String? = null,
    /** 该书籍支持压缩包内容浏览（M5：服务端下发，客户端不硬编码格式清单）。 */
    @SerialName("has_contents") val hasContents: Boolean,
    /** 媒体条目类型：audio | video（type=media 时存在）。 */
    @SerialName("media_kind") val mediaKind: String? = null,
    /** 媒体流地址（支持 Range，视频播放器用）。 */
    @SerialName("stream_url") val streamUrl: String? = null,
) {
    val isDir: Boolean get() = type == "dir"
    val isBook: Boolean get() = type == "book"
    val isImageFile: Boolean get() = type == "image"
    val isMedia: Boolean get() = type == "media"
    val isAudio: Boolean get() = mediaKind == "audio"
}

/** 书籍元数据 `GET /api/v1/books/{id}/meta`。 */
@Serializable
data class BookMetaDto(
    @SerialName("book_id") val bookId: String = "",
    val title: String = "",
    val author: String? = null,
    val format: String = "",
    val language: String? = null,
    val size: Long = 0,
    /** flow | fixed | strip */
    val mode: String = "flow",
    @SerialName("page_count") val pageCount: Int? = null,
    @SerialName("cover_url") val coverUrl: String? = null,
    @SerialName("render_support") val renderSupport: RenderSupportDto = RenderSupportDto(),
    val chapters: List<ChapterInfoDto> = emptyList(),
    val source: String? = null,
    /** 内容修订号，见 /layout 同名字段。 */
    val revision: String,
    /** 压缩包书籍才有：内容概览（设计 7.6）。 */
    val archive: ArchiveSummaryDto? = null,
)

@Serializable
data class RenderSupportDto(
    val png: Boolean = true,
    val json: Boolean = false,
)

@Serializable
data class ChapterInfoDto(
    val index: Int = 0,
    val title: String = "",
    @SerialName("total_chars") val totalChars: Int? = null,
    @SerialName("start_page") val startPage: Int? = null,
)

/** 文件夹图集 `POST /api/v1/books/open-folder`。 */
@Serializable
data class OpenFolderRequestDto(
    val path: String,
    val recursive: Boolean? = null,
)

@Serializable
data class OpenFolderResponseDto(
    @SerialName("book_id") val bookId: String = "",
    val title: String = "",
    val format: String = "gallery",
    @SerialName("page_count") val pageCount: Int = 0,
    @SerialName("cover_url") val coverUrl: String? = null,
    val mode: String = "fixed",
)

// ------------------------------------------------------------- 压缩包内容（设计 7.6）

/** 压缩包内容概览（meta.archive / contents.archive）。 */
@Serializable
data class ArchiveSummaryDto(
    @SerialName("entry_count") val entryCount: Int = 0,
    @SerialName("image_count") val imageCount: Int = 0,
    /** 当前书在嵌套链上的深度（顶层压缩包为 0）。 */
    val depth: Int = 0,
    @SerialName("max_depth") val maxDepth: Int = 2,
)

/** 压缩包内条目（/books/{id}/contents 列表行）。 */
@Serializable
data class ArchiveEntryItemDto(
    val name: String,
    /** dir | book | file | image */
    val type: String = "file",
    /** 包内完整条目路径（打开/预览用）。 */
    val path: String = "",
    val format: String? = null,
    val size: Long? = null,
    val gallery: GalleryInfoDto? = null,
    /** 图片条目的原始字节预览 URL（相对路径，需转绝对）。 */
    @SerialName("preview_url") val previewUrl: String? = null,
    val openable: Boolean = true,
    val reason: String? = null,
) {
    val isDir: Boolean get() = type == "dir"
    val isBook: Boolean get() = type == "book"
    val isImage: Boolean get() = type == "image"
}

@Serializable
data class ArchiveContentsResponseDto(
    @SerialName("book_id") val bookId: String = "",
    val revision: String = "",
    /** 当前包内目录（空串 = 包根）。 */
    val entry: String = "",
    /** 包内父目录（包根为 null，一级子目录为空串）。 */
    val parent: String? = null,
    val archive: ArchiveSummaryDto = ArchiveSummaryDto(),
    val entries: List<ArchiveEntryItemDto> = emptyList(),
    val total: Int = 0,
    val limit: Int = 500,
    val offset: Int = 0,
)

/** 包内条目打开 `POST /books/{id}/contents/open`。 */
@Serializable
data class OpenArchiveRequestDto(val entry: String)

@Serializable
data class OpenArchiveResponseDto(
    @SerialName("book_id") val bookId: String = "",
    val title: String = "",
    val format: String = "",
    /** book | image | dir */
    val kind: String = "book",
    val mode: String? = null,
    @SerialName("page_count") val pageCount: Int? = null,
    @SerialName("cover_url") val coverUrl: String? = null,
    val depth: Int = 1,
)
