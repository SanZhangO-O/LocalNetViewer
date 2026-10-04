package com.zqr.localnetviewer.data.prefs

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.zqr.localnetviewer.data.net.dto.AnchorDto
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import java.util.concurrent.ConcurrentHashMap

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

    // 按 bookId 串行化写入：DataStore.edit 只保证单次写原子，不保证两次并发 edit
    // 的先后次序。"旧阅读页的兜底落盘"与"重新打开同一本书的新写入"并发时，
    // 串行化保证后调用的 put 后落盘；调用方的兜底改为异步后，读取侧再由
    // pendingWrites 等待兜底写入完成，两者共同消灭旧页码回灌。
    private val locks = ConcurrentHashMap<String, Mutex>()

    // 兜底异步写入登记（View 销毁后由进程级 scope 落盘）：后续读取先等它完成，
    // 保证"退出阅读页后立刻重开同一本书"读到的是刚写入的位置，而不是旧值。
    private val pendingWrites = ConcurrentHashMap<String, Job>()

    private fun lock(bookId: String): Mutex = locks.getOrPut(bookId) { Mutex() }

    fun flow(bookId: String): Flow<ReadingProgress?> = store.data
        .onStart { pendingWrites[bookId]?.join() }
        .map { prefs ->
            prefs[key(bookId)]?.let { raw ->
                runCatching { prefsJson.decodeFromString<ReadingProgress>(raw) }.getOrNull()
            }
        }

    suspend fun get(bookId: String): ReadingProgress? = flow(bookId).first()

    /**
     * 异步兜底写入：登记为该书 pending，之后的 [get]/[flow] 会先等待它落盘。
     *
     * 与 [put] 只有调度方式不同（在调用方提供的进程级 scope 上执行），
     * 写入本身仍走同一把 per-book 锁与 DataStore 事务。
     */
    fun putAsync(scope: CoroutineScope, bookId: String, progress: ReadingProgress): Job {
        val job = scope.launch { put(bookId, progress) }
        pendingWrites[bookId] = job
        job.invokeOnCompletion { pendingWrites.remove(bookId, job) }
        return job
    }

    /** 进度写入是尽力而为：IO 失败不抛出，避免影响阅读（调用方不再捕获）。 */
    suspend fun put(bookId: String, progress: ReadingProgress) {
        runCatchingIo {
            lock(bookId).withLock {
                store.edit { prefs ->
                    prefs[key(bookId)] = prefsJson.encodeToString(
                        progress.copy(updatedAt = System.currentTimeMillis())
                    )
                }
            }
        }
    }

    suspend fun clear(bookId: String) {
        runCatchingIo {
            lock(bookId).withLock {
                store.edit { prefs -> prefs.remove(key(bookId)) }
            }
        }
    }

    private fun key(bookId: String) = stringPreferencesKey("progress_$bookId")
}
