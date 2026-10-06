package com.zqr.localnetviewer.ui.connect

import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.ApiClient
import com.zqr.localnetviewer.data.net.ApiException
import com.zqr.localnetviewer.data.prefs.ServerAddress
import com.zqr.localnetviewer.data.repo.LibraryRepository
import com.zqr.localnetviewer.data.runCatchingIo
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * 连接页状态机（设计 6.3 ConnectScreen）。
 *
 * 连接流程：`/server/info`（3s 超时，携带凭据）→ 需要凭据或已填写时再用 `/files`
 * 校验一次（区分"无法连接 / 需要密码 / 密码错误"）→ 保存地址与凭据 → 进入浏览页。
 */
class ConnectViewModel(app: App) : AndroidViewModel(app) {

    private val appRef = app
    private val settingsStore = app.settingsStore
    private val client = app.apiClient

    data class UiState(
        val address: String = "",
        val token: String = "",
        val connecting: Boolean = false,
        val error: String? = null,
        val serverName: String = "",
        val serverVersion: String = "",
        val librarySummary: String = "",
        val tokenRequired: Boolean = false,
        val connected: Boolean = false,
        val history: List<ServerAddress> = emptyList(),
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var connectJob: Job? = null
    private var connectSeq: Int = 0

    init {
        viewModelScope.launch {
            settingsStore.historyFlow.collect { list ->
                _state.update { it.copy(history = list) }
            }
        }
        viewModelScope.launch {
            val settings = settingsStore.current()
            _state.update { current ->
                current.copy(address = settings.serverUrl, token = settings.authToken)
            }
            // 只有进程内第一次进入连接页才自动重连：用户主动"断开"后不应被立刻连回去
            if (settings.serverUrl.isNotEmpty() && !app.autoConnectConsumed) {
                app.autoConnectConsumed = true
                connect(settings.serverUrl, settings.authToken)
            }
        }
    }

    fun onAddressChange(value: String) {
        _state.update { it.copy(address = value, error = null) }
    }

    fun onTokenChange(value: String) {
        _state.update { it.copy(token = value, error = null) }
    }

    fun useHistory(item: ServerAddress) {
        _state.update { it.copy(address = item.url, token = item.token) }
        connect(item.url, item.token)
    }

    fun forget(item: ServerAddress) {
        // 删除历史地址失败（IO）不影响主流程
        viewModelScope.launch {
            runCatchingIo { settingsStore.forgetServer(item.url, item.token) }
        }
    }

    fun consumeConnected() {
        _state.update { it.copy(connected = false) }
    }

    fun connect(address: String? = null, token: String? = null) {
        val rawAddress = address ?: _state.value.address
        val rawToken = token ?: _state.value.token
        val url = ApiClient.normalizeBaseUrl(rawAddress)
        if (url.isEmpty()) {
            _state.update { it.copy(error = "请输入服务器地址，例如 192.168.1.10") }
            return
        }
        val tokenValue = rawToken.trim()
        // 同一时刻只保留一次连接尝试：否则连点/点历史地址时，较早的一次可能最后返回，
        // 把共享的 ApiClient 与已保存地址改回旧服务器
        val seq = ++connectSeq
        connectJob?.cancel()
        connectJob = viewModelScope.launch {
            _state.update { it.copy(connecting = true, error = null) }
            try {
                // 用一次性客户端探测候选服务器（同样的地址规范化 / 3s 超时 / 令牌头）。
                // 探测失败或被取消时进程级 ApiClient 保持原配置不变；否则一个打错的
                // 地址会把全局客户端带偏，浏览页/阅读页全部跟着失败。
                val probeClient = ApiClient()
                probeClient.configure(url, tokenValue, ApiClient.PROBE_TIMEOUT_SECONDS)
                val probeRepository = LibraryRepository(probeClient)
                val info = probeRepository.serverInfo()
                // 凭据是书库密码（或管理员令牌）：auth_required=true 且未填写 → 先提示；
                // 填了就用 /files 验证一次（区分"密码错误"与其它失败）
                if (info.authRequired && tokenValue.isEmpty()) {
                    throw ApiException(ApiException.CODE_UNAUTHORIZED, "该书库需要访问密码，请填写后重试")
                }
                if (info.authRequired || tokenValue.isNotEmpty()) {
                    try {
                        probeRepository.listFiles("/", 1, 0)
                    } catch (unauthorized: ApiException) {
                        if (unauthorized.isUnauthorized) {
                            throw ApiException(unauthorized.code, "访问密码错误，请检查后重试")
                        }
                        throw unauthorized
                    }
                }
                if (seq != connectSeq) return@launch
                // 校验通过：才配置全局客户端并恢复常规超时（页面图片下载需要更长时间）
                client.configure(url, tokenValue, ApiClient.DEFAULT_CALL_TIMEOUT_SECONDS)
                val summary = info.libraries
                    .joinToString("、") { "${it.name}(${it.bookCount} 本)" }
                    .ifEmpty { "未配置书库" }
                // 地址/凭据落盘失败不影响本次连接；历史名带可见书库，便于按密码区分同一地址
                runCatchingIo {
                    settingsStore.setServer(url, tokenValue)
                    settingsStore.rememberServer(url, "${info.name} · $summary", tokenValue)
                }
                // 服务器信息放到 App 上：连接成功后本页立刻被移出返回栈，
                // 渲染在这里的"服务器信息卡片"用户根本看不到（设计 6.3）
                appRef.updateServerSummary("${info.name} v${info.version} · $summary")
                appRef.updateArchiveBrowse(info.features.archiveBrowse)
                _state.update {
                    it.copy(
                        connecting = false,
                        connected = true,
                        error = null,
                        address = url,
                        token = tokenValue,
                        serverName = info.name,
                        serverVersion = info.version,
                        librarySummary = summary,
                        tokenRequired = info.authRequired,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: ApiException) {
                val message = when {
                    error.isNetwork -> "无法连接：${error.message}"
                    error.isUnauthorized -> error.message
                    else -> error.message
                }
                if (seq == connectSeq) {
                    _state.update { it.copy(connecting = false, error = message) }
                }
            } catch (error: Exception) {
                if (seq == connectSeq) {
                    _state.update {
                        it.copy(connecting = false, error = error.message ?: "连接失败")
                    }
                }
            }
        }
    }
}
