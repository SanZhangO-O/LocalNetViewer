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

@Serializable
data class FileEntryDto(
    val name: String,
    /** dir | book | file | image */
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
) {
    val isDir: Boolean get() = type == "dir"
    val isBook: Boolean get() = type == "book"
    val isImageFile: Boolean get() = type == "image"
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
