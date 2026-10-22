package com.zqr.localnetviewer.data.prefs

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.zqr.localnetviewer.data.net.dto.AnchorDto
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

/** 每本书的书签（本地 DataStore，与进度同层：都只属于这台设备）。 */
private val Context.bookmarkDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "lnv_bookmarks"
)

/**
 * 一条书签。
 *
 * [anchor] 是**真正的定位依据**（改字号 / 转屏后页码会变，锚点不会）；
 * 页码只是给列表展示与"锚点也定位不到"时的兜底。握不住锚点时宁可不存页面图
 * 那种"精确到像素"的位置——那在重排后毫无意义。
 */
@Serializable
data class Bookmark(
    val id: String = "",
    val page: Int = 1,
    val anchor: AnchorDto? = null,
    val chapterTitle: String = "",
    /** 用户写的备注（可空；这是书签与"上次读到哪"的唯一区别）。 */
    val note: String = "",
    val createdAt: Long = 0L,
)

class BookmarkStore(private val context: Context) {

    private val store = context.bookmarkDataStore

    // 与 ProgressStore 同一套：DataStore.edit 只保证单次写原子，不保证两次并发
    // 读改写（"添加"与"删除"同时发生）的先后次序，按书串行化。
    private val locks = ConcurrentHashMap<String, Mutex>()

    private fun lock(bookId: String): Mutex = locks.getOrPut(bookId) { Mutex() }

    /** 书签按阅读顺序（页码升序）排列；同页按添加时间。 */
    fun flow(bookId: String): Flow<List<Bookmark>> = store.data.map { prefs ->
        decode(prefs[key(bookId)]).sortedWith(compareBy({ it.page }, { it.createdAt }))
    }

    suspend fun list(bookId: String): List<Bookmark> = flow(bookId).first()

    /**
     * 追加一条书签并返回新列表。
     *
     * 同一页同一锚点重复添加时直接覆盖备注，而不是堆两条一模一样的条目
     * （用户按两次收藏的意图通常是"改备注"）。
     */
    suspend fun add(bookId: String, bookmark: Bookmark): List<Bookmark>? {
        val entry = bookmark.copy(
            id = bookmark.id.ifEmpty { UUID.randomUUID().toString() },
            createdAt = if (bookmark.createdAt > 0L) {
                bookmark.createdAt
            } else {
                System.currentTimeMillis()
            },
        )
        return mutate(bookId) { current ->
            val existing = current.indexOfFirst {
                it.page == entry.page && it.anchor == entry.anchor
            }
            if (existing >= 0) {
                current.toMutableList().also { it[existing] = entry.copy(id = it[existing].id) }
            } else {
                (current + entry).toMutableList()
            }
        }
    }

    suspend fun remove(bookId: String, bookmarkId: String): List<Bookmark>? =
        mutate(bookId) { current -> current.filterNot { it.id == bookmarkId }.toMutableList() }

    /**
     * 读改写并返回新列表；写盘失败返回 null。
     *
     * 返回 null 而不是空列表：调用方若把"失败"当成"没有书签"更新 UI，一次磁盘
     * 抖动就会把列表清空（看起来像书签被吞了）。调用方据此保留原列表并提示重试。
     */
    private suspend fun mutate(
        bookId: String,
        transform: (List<Bookmark>) -> MutableList<Bookmark>,
    ): List<Bookmark>? {
        var result: List<Bookmark>? = null
        runCatchingIo {
            lock(bookId).withLock {
                store.edit { prefs ->
                    val current = decode(prefs[key(bookId)])
                    val next = transform(current)
                        .sortedWith(compareBy({ it.page }, { it.createdAt }))
                    prefs[key(bookId)] = prefsJson.encodeToString(next)
                    result = next
                }
            }
        }
        return result
    }

    private fun decode(raw: String?): List<Bookmark> {
        if (raw.isNullOrBlank()) return emptyList()
        return runCatching { prefsJson.decodeFromString<List<Bookmark>>(raw) }.getOrDefault(emptyList())
    }

    private fun key(bookId: String) = stringPreferencesKey("bookmarks_$bookId")
}
