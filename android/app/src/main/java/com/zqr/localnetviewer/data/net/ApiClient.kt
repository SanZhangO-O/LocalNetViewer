package com.zqr.localnetviewer.data.net

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import com.zqr.localnetviewer.BuildConfig
import kotlinx.coroutines.Deferred
import kotlinx.serialization.json.Json
import okhttp3.ConnectionPool
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import retrofit2.Retrofit
import java.util.concurrent.TimeUnit

/**
 * HTTP 客户端（设计 6.6）。
 *
 * - 地址与令牌运行期可变：连接成功后 [configure] 重建 Retrofit；
 * - 超时：连接 3s、读 15s，keep-alive 连接池；令牌统一由拦截器附加；
 * - 手动输入的是局域网 IP，无法按域名收敛，明文 HTTP 由 network_security_config 放行。
 */
class ApiClient {

    @Volatile
    private var baseUrl: String = ""

    @Volatile
    private var authToken: String? = null

    @Volatile
    private var currentTimeoutSeconds: Long = DEFAULT_CALL_TIMEOUT_SECONDS

    private var cachedService: ApiService? = null
    private val lock = Any()

    /** 进程重建后"恢复上次服务器地址"的任务（见 [api]）。 */
    @Volatile
    private var restoreJob: Deferred<*>? = null

    /** 供 Coil 复用的 OkHttp 实例（同一连接池 + HTTP 缓存）。 */
    val okHttpClient: OkHttpClient by lazy { buildOkHttp() }

    private val json: Json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
        encodeDefaults = true
        isLenient = true
    }

    // ------------------------------------------------------------ 配置
    fun isConfigured(): Boolean = baseUrl.isNotEmpty()

    fun currentBaseUrl(): String = baseUrl

    /**
     * 设置服务器地址（可带端口，缺省 8765）与令牌。
     *
     * @param callTimeoutSeconds 连接测试时传 3，让错误地址快速失败。
     */
    fun configure(
        serverUrl: String,
        token: String? = null,
        callTimeoutSeconds: Long = DEFAULT_CALL_TIMEOUT_SECONDS,
    ): ApiService {
        synchronized(lock) {
            baseUrl = normalizeBaseUrl(serverUrl)
            authToken = token?.trim()?.takeIf { it.isNotEmpty() }
            currentTimeoutSeconds = callTimeoutSeconds
            cachedService = null
            return serviceLocked()
        }
    }

    /** 取得当前 API（未配置地址时抛 [IllegalStateException]）。 */
    fun requireService(): ApiService {
        synchronized(lock) {
            return serviceLocked()
        }
    }

    /**
     * 取得当前 API；若地址尚未配置，先等"进程重建后恢复上次地址"的任务完成。
     *
     * 进程被系统回收后重建时，Navigation 会把界面直接还原到浏览页 / 阅读页，
     * 而服务器地址只存在于 DataStore —— 没有这一步，[requireService] 会抛
     * IllegalStateException，用户面对一个只能"断开连接"的错误页。
     */
    suspend fun api(): ApiService {
        if (baseUrl.isEmpty()) {
            restoreJob?.join()
        }
        return requireService()
    }

    /**
     * 由 [com.zqr.localnetviewer.App] 注入"从 DataStore 恢复地址"的任务。
     *
     * 用 [Deferred] 而不是在 onCreate 里跑一个普通协程：后者的完成时机对调用方不可见，
     * 恢复完成前的请求仍会失败。只用到 ``join()``，因此接受任意结果的 Deferred。
     */
    fun installRestore(job: Deferred<*>) {
        restoreJob = job
    }

    private fun serviceLocked(): ApiService {
        cachedService?.let { return it }
        check(baseUrl.isNotEmpty()) { "尚未配置服务器地址" }
        val client = if (currentTimeoutSeconds == DEFAULT_CALL_TIMEOUT_SECONDS) {
            okHttpClient
        } else {
            okHttpClient.newBuilder()
                .callTimeout(currentTimeoutSeconds, TimeUnit.SECONDS)
                .build()
        }
        val retrofit = Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory(JSON_MEDIA_TYPE))
            .build()
        val service = retrofit.create(ApiService::class.java)
        cachedService = service
        return service
    }

    /** 把服务器返回的相对路径（如 `/api/v1/books/x/cover`）变成绝对 URL。 */
    fun absoluteUrl(pathOrUrl: String): String {
        if (pathOrUrl.startsWith("http://") || pathOrUrl.startsWith("https://")) return pathOrUrl
        if (baseUrl.isEmpty()) return pathOrUrl
        val base = baseUrl.trimEnd('/')
        return if (pathOrUrl.startsWith("/")) base + pathOrUrl else "$base/$pathOrUrl"
    }

    fun decodeErrorBody(body: String?): ErrorPayload? {
        if (body.isNullOrBlank()) return null
        return try {
            val element = json.parseToJsonElement(body)
            val obj = element as? kotlinx.serialization.json.JsonObject ?: return null
            ErrorPayload(
                code = obj["code"]?.toString()?.trim('"') ?: "INTERNAL",
                message = obj["message"]?.toString()?.trim('"') ?: "",
            )
        } catch (_: Exception) {
            null
        }
    }

    // ------------------------------------------------------------ 内部
    private fun buildOkHttp(): OkHttpClient {
        val builder = OkHttpClient.Builder()
            .connectTimeout(CONNECT_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .readTimeout(READ_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .writeTimeout(READ_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .callTimeout(DEFAULT_CALL_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .retryOnConnectionFailure(true)
            .connectionPool(ConnectionPool(5, 5, TimeUnit.MINUTES))
            .addInterceptor { chain ->
                val request = chain.request().newBuilder()
                    .header("Accept", "application/json, image/*;q=0.8")
                    .apply { authToken?.let { header("X-Auth-Token", it) } }
                    .build()
                chain.proceed(request)
            }
        // 注意：这里**故意不挂 OkHttp Cache**。
        //
        // 页面图片是这条客户端上最大的流量，而 Coil 已经有一份 1GB 的磁盘缓存，
        // 且它复用的就是这个 OkHttp 实例。两边同时开启磁盘缓存的结果是每张页面图
        // 落盘两份（峰值 = OkHttp 256MB + Coil 1GB），却没有任何额外收益。
        // 服务器为页面下发了 immutable 强缓存头，Coil 的磁盘缓存足以兜底。
        if (BuildConfig.DEBUG) {
            builder.addInterceptor(
                HttpLoggingInterceptor().apply { level = HttpLoggingInterceptor.Level.BASIC }
            )
        }
        return builder.build()
    }

    data class ErrorPayload(val code: String, val message: String)

    companion object {
        const val DEFAULT_PORT = 8765
        const val CONNECT_TIMEOUT_SECONDS = 3L
        const val READ_TIMEOUT_SECONDS = 15L
        const val DEFAULT_CALL_TIMEOUT_SECONDS = 30L
        const val PROBE_TIMEOUT_SECONDS = 3L
        private val JSON_MEDIA_TYPE = "application/json".toMediaType()

        /**
         * 规范化用户输入：补 scheme、补默认端口、补结尾斜杠。
         *
         * `192.168.1.10` → `http://192.168.1.10:8765/`
         * `http://10.0.0.2:9000/books` → `http://10.0.0.2:9000/books/`
         */
        fun normalizeBaseUrl(raw: String): String {
            var input = raw.trim()
            if (input.isEmpty()) return ""
            if (!input.startsWith("http://") && !input.startsWith("https://")) {
                input = "http://$input"
            }
            val schemeEnd = input.indexOf("://") + 3
            if (schemeEnd < 3) return ""
            val scheme = input.substring(0, schemeEnd)
            val rest = input.substring(schemeEnd)
            val slashIndex = rest.indexOf('/')
            val authority = if (slashIndex >= 0) rest.substring(0, slashIndex) else rest
            val rawPath = if (slashIndex >= 0) rest.substring(slashIndex) else ""
            if (authority.isBlank()) return ""

            val brackets = authority.startsWith("[")
            val hasPort = if (brackets) {
                authority.substringAfter(']', "").startsWith(":")
            } else {
                authority.contains(':')
            }
            val hostPort = if (hasPort) authority else "$authority:$DEFAULT_PORT"

            val cleanPath = rawPath.trim('/')
            val suffix = if (cleanPath.isEmpty()) "" else "$cleanPath/"
            return "$scheme$hostPort/$suffix"
        }
    }
}
