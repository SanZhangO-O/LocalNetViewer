package com.zqr.localnetviewer

import android.app.Application
import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import coil3.ImageLoader
import coil3.PlatformContext
import coil3.SingletonImageLoader
import coil3.disk.DiskCache
import coil3.memory.MemoryCache
import coil3.network.okhttp.OkHttpNetworkFetcherFactory
import coil3.request.crossfade
import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.prefs.ProgressStore
import com.zqr.localnetviewer.data.prefs.SettingsStore
import com.zqr.localnetviewer.data.repo.FontStore
import com.zqr.localnetviewer.data.repo.LibraryRepository
import com.zqr.localnetviewer.data.repo.ReaderRepository
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import okio.Path.Companion.toOkioPath

/**
 * 应用入口与手写依赖容器（设计 6.2：App.kt 承载 DI）。
 *
 * 不引入 Hilt/Koin：依赖很少，手写容器更直观，也避免注解处理开销。
 */
class App : Application(), SingletonImageLoader.Factory {

    /**
     * 进程级协程域：供"比 ViewModel 活得更久"的小任务使用（启动恢复、兜底落盘）。
     *
     * 收敛为单一字段而不是每次 `CoroutineScope(...)` 现场创建：Application 存活
     * 期间它不取消，且便于审查"有哪些任务逃出了生命周期感知范围"。
     */
    val applicationScope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** 服务器地址 / 阅读设置 / 历史地址。 */
    val settingsStore: SettingsStore by lazy { SettingsStore(this) }

    /** 每本书的阅读进度（本地 DataStore）。 */
    val progressStore: ProgressStore by lazy { ProgressStore(this) }

    /** HTTP 客户端：地址可切换，令牌与超时统一管理。 */
    val apiClient: ApiClient by lazy { ApiClient() }

    override fun onCreate() {
        super.onCreate()
        // 进程被系统回收后重建：Navigation 会把界面直接还原到浏览页 / 阅读页，
        // 而服务器地址只存在于 DataStore（ApiClient 的内存字段是空的）。
        // 这里在进程启动时就把地址恢复出来，并把任务交给 ApiClient ——
        // 恢复完成前的请求会挂在 api() 上等待，而不是直接抛 IllegalStateException。
        val job = applicationScope.async {
            runCatchingIo {
                val settings = settingsStore.current()
                // 用户在连接页抢先输入了新地址时不覆盖它
                if (settings.serverUrl.isNotEmpty() && !apiClient.isConfigured()) {
                    apiClient.configure(settings.serverUrl, settings.authToken)
                }
            }
        }
        apiClient.installRestore(job)
    }

    val libraryRepository: LibraryRepository by lazy {
        LibraryRepository(apiClient)
    }

    val readerRepository: ReaderRepository by lazy {
        ReaderRepository(apiClient)
    }

    /** 文字直发模式：服务器字体下载与缓存（设计 3.2）。 */
    val fontStore: FontStore by lazy {
        FontStore(this, apiClient)
    }

    /**
     * 是否已经用掉"启动自动重连"这一次机会。
     *
     * 只有进程内第一次进入连接页才自动重连上次地址；用户主动"断开"后回到连接页
     * 不应该被立刻又连回去（设计 6.3：历史地址由用户点击重连）。
     */
    @Volatile
    var autoConnectConsumed: Boolean = false

    /**
     * 已连接服务器的概要（"名称 v版本 · 书库(本数)"），浏览页顶栏显示（设计 6.3）。
     *
     * 放在 App 而不是 ConnectScreen 里：连接成功后连接页会立刻被移出返回栈，
     * 该页上渲染的"服务器信息卡片"用户根本看不到。
     */
    var serverSummary: String by mutableStateOf("")
        private set

    fun updateServerSummary(text: String) {
        serverSummary = text
    }

    /**
     * 当前服务器是否支持压缩包内容浏览（设计 7.6，features.archive_browse）。
     * 连接探测成功时更新；断开时不主动复位（界面只在有服务器摘要时显示入口）。
     */
    var archiveBrowseAvailable: Boolean by mutableStateOf(false)
        private set

    fun updateArchiveBrowse(available: Boolean) {
        archiveBrowseAvailable = available
    }

    /**
     * Coil 3 图片加载器：复用 OkHttp（与接口同一连接池）+ 磁盘缓存兜底页面图片。
     *
     * 页面图片的强缓存头（immutable）由服务器下发，Coil 磁盘缓存做二次兜底，
     * 因此服务器端页面缓存不需要落盘（设计 3.4）。
     */
    override fun newImageLoader(context: PlatformContext): ImageLoader {
        return ImageLoader.Builder(context)
            .components {
                add(OkHttpNetworkFetcherFactory(callFactory = { apiClient.okHttpClient }))
            }
            .memoryCache {
                MemoryCache.Builder()
                    .maxSizePercent(context, 0.25)
                    .build()
            }
            .diskCache {
                DiskCache.Builder()
                    .directory(cacheDir.resolve("page_cache").toOkioPath())
                    .maxSizeBytes(DEFAULT_DISK_CACHE_BYTES)
                    .build()
            }
            .crossfade(false)
            .build()
    }

    companion object {
        /** 页面图片磁盘缓存上限（设计 6.4：Coil 磁盘缓存按 LRU 自动淘汰旧 layout）。 */
        const val DEFAULT_DISK_CACHE_BYTES = 1024L * 1024L * 1024L

        fun from(context: Context): App = context.applicationContext as App
    }
}
