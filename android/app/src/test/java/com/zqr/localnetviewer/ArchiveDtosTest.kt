package com.zqr.localnetviewer

import com.zqr.localnetviewer.data.net.dto.ArchiveContentsResponseDto
import com.zqr.localnetviewer.data.net.dto.BookMetaDto
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 压缩包内容浏览（设计 7.6）DTO 契约：服务器 JSON 字段（snake_case）必须与
 * 客户端 @SerialName 一一对应。
 */
class ArchiveDtosTest {

    private val json = Json { ignoreUnknownKeys = true }

    @Test
    fun `contents 响应解析 snake_case 字段`() {
        val payload = """
            {
              "book_id": "abc123",
              "revision": "rev1",
              "entry": "vol01",
              "parent": "",
              "archive": {"entry_count": 12, "image_count": 10, "depth": 0, "max_depth": 2},
              "entries": [
                {"name": "001.png", "type": "image", "path": "vol01/001.png",
                 "format": "gallery", "size": 1024,
                 "gallery": {"image_count": 1, "can_open_as_book": true},
                 "preview_url": "/api/v1/books/abc123/asset?path=x", "openable": true},
                {"name": "novel.txt", "type": "book", "path": "novel.txt", "format": "txt"},
                {"name": "sub", "type": "dir", "path": "vol01/sub",
                 "gallery": {"image_count": 3, "can_open_as_book": true}},
                {"name": "data.bin", "type": "file", "openable": false, "reason": "非电子书格式"}
              ],
              "total": 4, "limit": 200, "offset": 0
            }
        """.trimIndent()
        val body = json.decodeFromString(ArchiveContentsResponseDto.serializer(), payload)
        assertEquals("abc123", body.bookId)
        assertEquals("rev1", body.revision)
        assertEquals("vol01", body.entry)
        assertEquals("", body.parent)
        assertEquals(12, body.archive.entryCount)
        assertEquals(10, body.archive.imageCount)
        val image = body.entries[0]
        assertTrue(image.isImage)
        assertEquals("/api/v1/books/abc123/asset?path=x", image.previewUrl)
        assertTrue(body.entries[1].isBook)
        assertEquals("txt", body.entries[1].format)
        assertTrue(body.entries[2].isDir)
        assertTrue(body.entries[2].gallery?.canOpenAsBook == true)
        val file = body.entries[3]
        assertFalse(file.openable)
        assertEquals("非电子书格式", file.reason)
    }

    @Test
    fun `meta 的 archive 块缺省为 null`() {
        val withArchive = json.decodeFromString(
            BookMetaDto.serializer(),
            """{"book_id":"a","title":"t","format":"zip","revision":"rev1",
                "archive":{"entry_count":3,"image_count":1,"depth":1,"max_depth":2}}""",
        )
        assertEquals(1, withArchive.archive?.depth)
        val withoutArchive = json.decodeFromString(
            BookMetaDto.serializer(),
            """{"book_id":"a","title":"t","format":"txt","revision":"rev1"}""",
        )
        assertNull(withoutArchive.archive)
    }
}
