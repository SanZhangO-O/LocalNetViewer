package com.zqr.localnetviewer.data

import kotlinx.coroutines.CancellationException

/**
 * `runCatching` 的协程安全版本：吞掉 IO 异常，但把 [CancellationException] 原样抛出。
 *
 * 为什么不能用 `runCatching`：DataStore 的 `edit {}` 是可取消的挂起函数，
 * `runCatching` 会把协程取消当成"普通失败"一起吞掉 —— 调用方于是无法区分
 * "已经写入磁盘" 与 "在写入途中被取消（其实没写）"。
 *
 * 阅读进度保存正是这个形态：用户翻页后 800ms 防抖落盘，此时退出阅读页会取消
 * `viewModelScope`，旧写法会静默返回，看起来"存过了"。
 */
inline fun <T> runCatchingIo(block: () -> T): Result<T> =
    try {
        Result.success(block())
    } catch (cancel: CancellationException) {
        throw cancel
    } catch (error: Throwable) {
        Result.failure(error)
    }
