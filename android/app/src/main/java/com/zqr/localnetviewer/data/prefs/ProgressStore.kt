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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOn
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
    /**
     * 书架展示用的快照字段（旧版本写入的记录没有它们，缺省为空）。
     *
     * 存进进度而不是另建一张"最近阅读"表：一本书只有一条记录，"读到哪"和
     * "这是什么书"天然同生命周期，少一份需要同步的草稿。
     */
    val title: String = "",
    val format: String = "",
    val vpath: String = "",
    /** 上次已知总页数（0 = 未知）：continue-reading 卡片用它显示"第 42 页"。 */
    val totalPages: Int = 0,
)

/** 一条"最近阅读"：book_id 与它当前的进度快照。 */
data class RecentBook(
    val bookId: String,
    val progress: ReadingProgress,
) {
    val displayTitle: String
        get() = progress.title.ifEmpty {
            progress.vpath.trimEnd('/').substringAfterLast('/').ifEmpty { bookId }
        }

    /** 0..1；未知（旧记录没有比例）时为 null。 */
    val fraction: Float?
        get() = progress.ratio.takeIf { it in 0.0001f..1f }
}

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
     * 最近阅读（按落盘时间倒序，最多 [limit] 本）。
     *
     * 直接遍历 DataStore 的键而不是再维护一份索引：键前缀本身就是 book_id，
     * 记录数上限就是"读过的书数"，量级与书架相当，启动时读一遍足够便宜。
     */
    fun recent(limit: Int = 20): Flow<List<RecentBook>> =
        store.data
            .onStart { pendingWrites.values.toList().forEach { it.join() } }
            .map { prefs ->
                prefs.asMap().entries
                    .mapNotNull { (key, value) ->
                        val name = key.name
                        if (!name.startsWith(KEY_PREFIX) || value !is String) return@mapNotNull null
                        val progress = runCatching {
                            prefsJson.decodeFromString<ReadingProgress>(value)
                        }.getOrNull() ?: return@mapNotNull null
                        if (progress.updatedAt <= 0L) return@mapNotNull null
                        RecentBook(name.removePrefix(KEY_PREFIX), progress)
                    }
                    .sortedByDescending { it.progress.updatedAt }
                    .take(limit)
            }
            // 解码 N 条 JSON 是纯 CPU 活：书架每次 ON_RESUME 都会读一遍，
            // 不该占主线程（DataStore 自身的 IO 调度不覆盖 map 里的解码）
            .flowOn(Dispatchers.IO)

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

    private fun key(bookId: String) = stringPreferencesKey("$KEY_PREFIX$bookId")

    private companion object {
        const val KEY_PREFIX = "progress_"
    }
}
