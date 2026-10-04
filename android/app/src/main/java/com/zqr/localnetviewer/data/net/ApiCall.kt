package com.zqr.localnetviewer.data.net

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerializationException
import retrofit2.HttpException
import java.io.IOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException

/**
 * 统一错误：把 HTTP 错误体（`{"code","message"}`）、网络异常与超时归一化，
 * UI 只需看 [code] 与 [message]（设计 5.1 错误码表）。
 */
class ApiException(
    val code: String,
    override val message: String,
    val httpStatus: Int = 0,
) : Exception(message) {

    /** 旧 layout 失效：客户端应重新 POST /layout 并按锚点定位（设计 6.4）。 */
    val isLayoutLost: Boolean get() = code == CODE_LAYOUT_NOT_FOUND

    /** 页码越界：固定版式内容变化，客户端应提示刷新。 */
    val isPageOutOfRange: Boolean get() = code == CODE_PAGE_OUT_OF_RANGE

    val isNetwork: Boolean get() = code == CODE_NETWORK

    val isUnauthorized: Boolean get() = code == CODE_UNAUTHORIZED

    companion object {
        const val CODE_NETWORK = "NETWORK"
        const val CODE_TIMEOUT = "TIMEOUT"
        const val CODE_UNAUTHORIZED = "UNAUTHORIZED"
        const val CODE_LAYOUT_NOT_FOUND = "LAYOUT_NOT_FOUND"
        const val CODE_PAGE_OUT_OF_RANGE = "PAGE_OUT_OF_RANGE"
        const val CODE_BOOK_NOT_FOUND = "BOOK_NOT_FOUND"
        /** 错误体缺失/不可解析时的 404 兜底码：不能臆断为 BOOK_NOT_FOUND（会掩盖 layout 失效恢复）。 */
        const val CODE_NOT_FOUND = "NOT_FOUND"
        const val CODE_PARSE_FAILED = "PARSE_FAILED"
        const val CODE_INTERNAL = "INTERNAL"

        fun of(throwable: Throwable, client: ApiClient? = null): ApiException {
            return when (throwable) {
                is ApiException -> throwable
                is SocketTimeoutException -> ApiException(
                    CODE_TIMEOUT, "连接超时，请检查地址与局域网是否互通"
                )
                is ConnectException -> ApiException(
                    CODE_NETWORK, "无法连接服务器，请确认服务器已启动且在同一局域网"
                )
                is UnknownHostException -> ApiException(CODE_NETWORK, "地址无法解析：${throwable.message}")
                // 反序列化失败 = 服务器返回的字段与客户端 DTO 不匹配（版本不一致），
                // 不能算"服务器内部错误"：用户看到 kotlinx 的原文（"Unexpected JSON token
                // at offset 123…"）既看不懂，也无法与真正的 500 区分
                is SerializationException -> ApiException(
                    CODE_PARSE_FAILED,
                    "服务器返回的数据格式与客户端不匹配（请确认服务器与 App 版本一致）",
                )
                is IllegalStateException -> ApiException(CODE_INTERNAL, throwable.message ?: "客户端未就绪")
                is IOException -> ApiException(CODE_NETWORK, "网络错误：${throwable.message ?: "连接中断"}")
                is HttpException -> fromHttp(throwable, client)
                else -> ApiException(CODE_INTERNAL, throwable.message ?: "未知错误")
            }
        }

        fun fromHttp(error: HttpException, client: ApiClient?): ApiException {
            val status = error.code()
            val raw = runCatching { error.response()?.errorBody()?.string() }.getOrNull()
            val parsed = client?.decodeErrorBody(raw)
            val code = parsed?.code ?: when (status) {
                401 -> CODE_UNAUTHORIZED
                // 服务器通常返回 LAYOUT_NOT_FOUND / PAGE_OUT_OF_RANGE 等结构化错误体；
                // 拿不到错误体时只能给"未知的 404"，把 BOOK_NOT_FOUND 留给明确语义的场景
                404 -> CODE_NOT_FOUND
                else -> "HTTP_$status"
            }
            val message = parsed?.message?.takeIf { it.isNotBlank() } ?: defaultMessage(status)
            return ApiException(code, message, status)
        }

        private fun defaultMessage(status: Int): String = when (status) {
            401 -> "需要访问令牌或令牌错误"
            403 -> "服务器拒绝访问该路径"
            404 -> "资源不存在"
            415 -> "服务器不支持该格式"
            422 -> "服务器解析失败"
            in 500..599 -> "服务器内部错误（$status）"
            else -> "请求失败（$status）"
        }
    }
}

/** 统一包裹 suspend 调用：把异常翻译成 [ApiException]，取消异常原样抛出。 */
suspend fun <T> apiCall(client: ApiClient? = null, block: suspend () -> T): T {
    return try {
        block()
    } catch (cancel: CancellationException) {
        throw cancel
    } catch (throwable: Throwable) {
        throw if (throwable is HttpException) {
            // errorBody().string() 是网络 IO：放 IO 线程，避免大错误体 / 慢 socket
            // 卡住调用方（通常是 Main）调度器
            withContext(Dispatchers.IO) { ApiException.of(throwable, client) }
        } else {
            ApiException.of(throwable, client)
        }
    }
}
