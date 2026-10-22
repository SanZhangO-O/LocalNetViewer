package com.zqr.localnetviewer.ui.reader

import com.zqr.localnetviewer.data.net.dto.PageJsonDto
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.abs

/**
 * 文字直发（JSON 页）的页缓存状态机。
 *
 * 从 ViewModel 里拆出来的原因：这一小块有三条容易写错、且与网络/协程无关的规则，
 * 混在 900 行的 ViewModel 里只能靠注释维持：
 *
 * 1. **在途去重看请求，不看 UI 状态** —— 预取被翻页取消时 `Loading` 会留在
 *    `textPages` 里；拿它当"在途"就会永久拒绝重取（页面无限转圈且没有重试入口）。
 * 2. **缓存有上限** —— 长时间阅读不能把整本书的 JSON 攒在内存里，只留离当前页
 *    最近的 [capacity] 页。
 * 3. **失败要留占位** —— 直接删条目会让页面退化成"永远转圈、没有重试入口"；
 *    留一个 Failed 占位，用户点得到"重试"，重排成功也会整体清空。
 *
 * 纯 JVM 类（无 Android 依赖），因此可以被普通单元测试覆盖。
 */
class TextPageCache(private val capacity: Int = DEFAULT_CAPACITY) {

    private val inFlight: MutableSet<Int> = ConcurrentHashMap.newKeySet()

    /**
     * 登记"该页开始取"。返回 false 表示不该发请求（已有内容 / 已在途）。
     *
     * [pages] 由调用方传入当前快照：判定"已有内容"必须在同一份快照上做，
     * 否则与正在提交的 update 交错时会漏判。
     */
    fun begin(page: Int, pages: Map<Int, TextPageState>, force: Boolean = false): Boolean {
        if (!force && pages[page] is TextPageState.Ready) return false
        return inFlight.add(page)
    }

    /** 请求结束（成功、失败、取消都必须调用，否则该页再也取不到）。 */
    fun finish(page: Int) {
        inFlight.remove(page)
    }

    /** 放入 Loading 占位（已按当前页裁剪）。 */
    fun loading(
        pages: Map<Int, TextPageState>,
        page: Int,
        currentPage: Int,
    ): Map<Int, TextPageState> = prune(pages + (page to TextPageState.Loading), currentPage)

    /** 放入成功结果（已按当前页裁剪）。 */
    fun ready(
        pages: Map<Int, TextPageState>,
        page: Int,
        json: PageJsonDto,
        currentPage: Int,
    ): Map<Int, TextPageState> =
        prune(pages + (page to TextPageState.Ready(json)), currentPage)

    /** 放入失败占位（已按当前页裁剪）。 */
    fun failed(
        pages: Map<Int, TextPageState>,
        page: Int,
        message: String,
        currentPage: Int,
    ): Map<Int, TextPageState> =
        prune(pages + (page to TextPageState.Failed(message)), currentPage)

    /** 取消时抹掉 Loading 残影；已经 Ready / Failed 的条目保持不动。 */
    fun dropLoading(pages: Map<Int, TextPageState>, page: Int): Map<Int, TextPageState> =
        if (pages[page] is TextPageState.Loading) pages - page else pages

    /** 只保留离 [currentPage] 最近的 [capacity] 页。 */
    fun prune(pages: Map<Int, TextPageState>, currentPage: Int): Map<Int, TextPageState> {
        if (pages.size <= capacity) return pages
        return pages.entries
            .sortedBy { abs(it.key - currentPage) }
            .take(capacity)
            .associate { it.key to it.value }
    }

    companion object {
        /** 默认缓存页数：一屏前后各几页，够预取与回翻，又不至于攒整本书。 */
        const val DEFAULT_CAPACITY = 12
    }
}
