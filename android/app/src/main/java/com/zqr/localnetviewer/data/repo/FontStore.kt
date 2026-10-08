package com.zqr.localnetviewer.data.repo

import android.content.Context
import android.graphics.Typeface
import android.net.Uri
import android.util.Log
import com.zqr.localnetviewer.data.net.ApiClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import okhttp3.Request
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/**
 * 服务器字体缓存（设计 3.2 / 5.2 #14：JSON 模式客户端对齐排版用）。
 *
 * 文字直发模式下客户端逐行绘制服务器下发的行，行宽要尽量与服务器一致，否则个别行
 * 会溢出可用宽度。因此按字体 id 从 `/render/fonts/{id}/file` 下载字体文件缓存到
 * 应用私有目录，再用 [Typeface] 交给 Compose；下载失败 / 字体不存在时返回 null，
 * 由调用方回退系统字体并做横向压缩兜底。
 */
class FontStore(private val context: Context, private val apiClient: ApiClient) {

    private val memory = HashMap<String, Typeface>()
    private val locks = ConcurrentHashMap<String, Mutex>()

    /** 失败冷却（font id → 可重试时间戳）：下载失败时短期不再重试，避免每翻一页重下一次。 */
    private val failedUntil = ConcurrentHashMap<String, Long>()

    /** CJK 字体可达十几 MB，单开长超时的客户端，避免默认 30s callTimeout 中断下载。 */
    private val downloadClient by lazy {
        apiClient.okHttpClient.newBuilder()
            .callTimeout(DOWNLOAD_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .readTimeout(DOWNLOAD_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .build()
    }

    /** 已加载到内存的字体（组合阶段可直接取，不触发下载）。 */
    fun cached(fontId: String): Typeface? = synchronized(memory) { memory[fontId] }

    /**
     * 取字体：内存 → 磁盘缓存 → 下载。任何失败都返回 null（调用方回退系统字体），
     * 不抛异常——文字直发是"尽量"模式，不能因为字体问题挡住阅读。
     */
    suspend fun load(fontId: String): Typeface? {
        val id = fontId.trim()
        if (id.isEmpty()) return null
        cached(id)?.let { return it }
        if (isCoolingDown(id)) return null
        val mutex = locks.getOrPut(id) { Mutex() }
        return mutex.withLock {
            cached(id)?.let { return@withLock it }
            if (isCoolingDown(id)) return@withLock null
            val file = fileFor(id)
            if (!file.isFile && !download(id, file)) {
                failedUntil[id] = System.currentTimeMillis() + FAIL_COOLDOWN_MS
                return@withLock null
            }
            val typeface = withContext(Dispatchers.IO) {
                runCatching { Typeface.createFromFile(file) }.getOrNull()
            }
            if (typeface == null) {
                // 损坏缓存（下载不完整 / 不支持的字体容器）：清掉，下次重新下载
                file.delete()
                failedUntil[id] = System.currentTimeMillis() + FAIL_COOLDOWN_MS
                return@withLock null
            }
            failedUntil.remove(id)
            synchronized(memory) { memory[id] = typeface }
            typeface
        }
    }

    /** 失败冷却期内直接返回，不再发起下载（也无需排队等 mutex）。 */
    private fun isCoolingDown(id: String): Boolean {
        val until = failedUntil[id] ?: return false
        if (System.currentTimeMillis() >= until) {
            failedUntil.remove(id)
            return false
        }
        return true
    }

    private fun fileFor(id: String): File {
        val dir = File(context.filesDir, "fonts")
        if (!dir.exists() && !dir.mkdirs()) {
            Log.w(TAG, "字体缓存目录创建失败：${dir.path}")
        }
        return File(dir, id.replace(UNSAFE_FILE_CHARS, "_"))
    }

    private suspend fun download(id: String, target: File): Boolean = withContext(Dispatchers.IO) {
        val url = apiClient.absoluteUrl("/api/v1/render/fonts/${Uri.encode(id)}/file")
        runCatching {
            downloadClient.newCall(Request.Builder().url(url).build()).execute().use { response ->
                val body = response.body ?: return@use false
                if (!response.isSuccessful) return@use false
                // 先写 .part 再改名：中途失败不会留下一个会被当成有效缓存的半截文件
                val part = File(target.parentFile, "${target.name}.part")
                try {
                    body.byteStream().use { input ->
                        part.outputStream().use { output -> input.copyTo(output) }
                    }
                    if (!part.renameTo(target)) {
                        part.copyTo(target, overwrite = true)
                        part.delete()
                    }
                    true
                } finally {
                    if (part.exists()) part.delete()
                }
            }
        }.getOrElse { error ->
            Log.w(TAG, "字体下载失败（回退系统字体）：$id，${error.message}")
            false
        }
    }

    private companion object {
        const val TAG = "FontStore"
        const val DOWNLOAD_TIMEOUT_SECONDS = 180L

        /** 字体获取失败后的冷却期：期间用系统字体兜底，不再发起下载。 */
        const val FAIL_COOLDOWN_MS = 60_000L
        val UNSAFE_FILE_CHARS = Regex("[^A-Za-z0-9._-]")
    }
}
