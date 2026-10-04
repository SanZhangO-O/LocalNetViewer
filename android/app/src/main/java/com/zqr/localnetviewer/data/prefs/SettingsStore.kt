package com.zqr.localnetviewer.data.prefs

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/** 设置与历史地址共用一个 DataStore（设计 6.2）。 */
private val Context.settingsDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "lnv_settings"
)

internal val prefsJson = Json {
    ignoreUnknownKeys = true
    encodeDefaults = true
    explicitNulls = false
}

/**
 * 阅读设置（本地权威）。
 *
 * 字号 / 行距 / 边距等以 sp、dp 存储（符合设置界面的语义），
 * 在上报排版参数时按 density 换算成物理像素（设计 4.6）。
 */
@Serializable
data class AppSettings(
    val fontSizeSp: Int = 18,
    val lineHeight: Float = 1.6f,
    val marginHorizontalDp: Int = 16,
    val marginVerticalDp: Int = 8,
    val paragraphSpacingSp: Int = 6,
    /** white | paper | sepia | green | dark */
    val theme: String = "white",
    /** 字体 id（"auto" 表示服务器默认字体） */
    val fontFamily: String = "auto",
    /** ltr（美漫/港漫）| rtl（日漫） */
    val readingDirection: String = "ltr",
    /** screen（铺满屏幕，默认）| page（整页）| width（适应宽度）| height（适应高度） */
    val fitMode: String = "screen",
    /** 固定版式取原图（fit=original，保真但流量大） */
    val originalQuality: Boolean = false,
    /** 条漫切片连续滚动 */
    val stripMode: Boolean = false,
    /** 双页跨页（M4） */
    val pairPages: Boolean = false,
    val keepScreenOn: Boolean = true,
    /**
     * 界面预设：eink（墨水屏，默认）| lcd（常规屏，动画过渡）| custom（自定义）。
     *
     * 墨水屏预设下所有界面过渡直接落位（翻页跳转、工具栏显隐、列表滚动）：
     * 渐变/滑动动画在墨水屏上表现为多次灰阶全刷 + 残影。
     */
    val uiPreset: String = "eink",
    /** 仅 custom 预设生效：是否启用界面过渡动画。 */
    val animatedTransitions: Boolean = false,
    /** 上次连接的服务器地址与令牌 */
    val serverUrl: String = "",
    val authToken: String = "",
) {
    /** 当前预设是否使用动画过渡（eink=否，lcd=是，custom=看开关）。 */
    val animateTransitions: Boolean
        get() = when (uiPreset) {
            PRESET_EINK -> false
            PRESET_LCD -> true
            else -> animatedTransitions
        }
    /** 只有几何相关的设置变化才需要重新分页（设计 3.3 / 6.4）。 */
    fun geometrySignature(): String = listOf(
        fontSizeSp,
        lineHeight,
        marginHorizontalDp,
        marginVerticalDp,
        paragraphSpacingSp,
        fontFamily,
        stripMode,
    ).joinToString("|")

    companion object {
        val DEFAULT = AppSettings()
        const val PRESET_EINK = "eink"
        const val PRESET_LCD = "lcd"
        const val PRESET_CUSTOM = "custom"
    }
}

/** 历史服务器地址。 */
@Serializable
data class ServerAddress(
    val url: String,
    val name: String = "",
    val token: String = "",
    val lastUsed: Long = 0L,
)

/**
 * 服务器地址历史 + 阅读设置（DataStore Preferences）。
 *
 * 不引入 Room：进度与设置都是小数据（设计 6.1）。
 *
 * 所有"读-改-写"都在 `store.edit { }` 事务**内部**完成：先在事务外 `first()` 读快照、
 * 再 `edit` 写回，会与并发的另一次写互相覆盖（用户在连接页点连接的同时删掉一条历史地址，
 * 删除结果会被写回去）。
 */
class SettingsStore(private val context: Context) {

    private val store = context.settingsDataStore

    val settingsFlow: Flow<AppSettings> = store.data.map { prefs -> decodeSettings(prefs[KEY_SETTINGS]) }

    val historyFlow: Flow<List<ServerAddress>> = store.data.map { prefs ->
        decodeHistory(prefs[KEY_HISTORY])
    }

    suspend fun current(): AppSettings = settingsFlow.first()

    /**
     * 整体覆盖写设置。
     *
     * 变换在事务内基于**磁盘上的当前值**执行；落盘失败（磁盘满 / IO 异常）时仍返回新值，
     * 调用方无需处理异常（UI 已经按新值渲染）。
     */
    suspend fun update(transform: (AppSettings) -> AppSettings): AppSettings {
        // 返回值必须与事务内基于磁盘当前值算出的结果一致：事务外先 current()
        // 可能拿到并发写入前的过期快照，返回它就会与"实际落盘值"不一致
        var next: AppSettings? = null
        runCatchingIo {
            store.edit { prefs ->
                val updated = transform(decodeSettings(prefs[KEY_SETTINGS]))
                next = updated
                prefs[KEY_SETTINGS] = prefsJson.encodeToString(updated)
            }
        }
        // 落盘失败（磁盘满等）时退回当前磁盘快照，保持尽力而为的既有语义
        return next ?: current()
    }

    suspend fun setServer(url: String, token: String): AppSettings =
        update { it.copy(serverUrl = url, authToken = token) }

    /** 记住地址（同一地址只保留一条，置顶并按最近使用排序）。 */
    suspend fun rememberServer(url: String, name: String, token: String) {
        val now = System.currentTimeMillis()
        runCatchingIo {
            store.edit { prefs ->
                val existing = decodeHistory(prefs[KEY_HISTORY])
                val updated = buildList {
                    add(ServerAddress(url = url, name = name, token = token, lastUsed = now))
                    addAll(existing.filterNot { it.url == url })
                }.sortedByDescending { it.lastUsed }.take(MAX_HISTORY)
                prefs[KEY_HISTORY] = prefsJson.encodeToString(updated)
            }
        }
    }

    suspend fun forgetServer(url: String) {
        runCatchingIo {
            store.edit { prefs ->
                val remaining = decodeHistory(prefs[KEY_HISTORY]).filterNot { it.url == url }
                prefs[KEY_HISTORY] = prefsJson.encodeToString(remaining)
            }
        }
    }

    private fun decodeSettings(raw: String?): AppSettings {
        if (raw == null) return AppSettings.DEFAULT
        return runCatching { prefsJson.decodeFromString<AppSettings>(raw) }.getOrNull()
            ?: AppSettings.DEFAULT
    }

    private fun decodeHistory(raw: String?): List<ServerAddress> {
        if (raw == null) return emptyList()
        return runCatching { prefsJson.decodeFromString<List<ServerAddress>>(raw) }.getOrNull()
            ?: emptyList()
    }

    private companion object {
        const val MAX_HISTORY = 12
        val KEY_SETTINGS = stringPreferencesKey("app_settings")
        val KEY_HISTORY = stringPreferencesKey("server_history")
    }
}
