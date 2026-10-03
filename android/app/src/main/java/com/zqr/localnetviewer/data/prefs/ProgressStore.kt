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
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString

/** 每本书的阅读进度（设计 6.5）：只存锚点，页码仅供展示与快速定位。 */
private val Context.progressDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "lnv_progress"
)

/**
 * 每本书的阅读进度（设计 6.5）。
 *
 * ``anchor`` 允许为空：只有"页元数据确实属于该页"时才记录锚点。若拿不到匹配的锚点，
 * 就退回用 ``ratio`` / ``page`` 恢复，避免用错锚点把读者带到错误的位置。
 */
@Serializable
data class ReadingProgress(
    val anchor: AnchorDto? = null,
    val page: Int = 1,
    val ratio: Float = 0f,
    val updatedAt: Long = 0L,
)

class ProgressStore(private val context: Context) {

    private val store = context.progressDataStore

    fun flow(bookId: String): Flow<ReadingProgress?> = store.data.map { prefs ->
        prefs[key(bookId)]?.let { raw ->
            runCatching { prefsJson.decodeFromString<ReadingProgress>(raw) }.getOrNull()
        }
    }

    suspend fun get(bookId: String): ReadingProgress? = flow(bookId).first()

    /** 进度写入是尽力而为：IO 失败不抛出，避免影响阅读（调用方不再捕获）。 */
    suspend fun put(bookId: String, progress: ReadingProgress) {
        runCatchingIo {
            store.edit { prefs ->
                prefs[key(bookId)] = prefsJson.encodeToString(
                    progress.copy(updatedAt = System.currentTimeMillis())
                )
            }
        }
    }

    suspend fun clear(bookId: String) {
        runCatchingIo {
            store.edit { prefs -> prefs.remove(key(bookId)) }
        }
    }

    private fun key(bookId: String) = stringPreferencesKey("progress_$bookId")
}
