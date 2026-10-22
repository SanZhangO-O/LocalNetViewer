package com.zqr.localnetviewer

import com.zqr.localnetviewer.data.net.dto.PageJsonDto
import com.zqr.localnetviewer.ui.reader.TextPageCache
import com.zqr.localnetviewer.ui.reader.TextPageState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 文字直发页缓存的三条不变量（从 ReaderViewModel 拆出来时锁住的回归）：
 * 在途去重看请求不看 UI 状态、缓存按离当前页的远近裁剪、失败保留占位。
 */
class TextPageCacheTest {

    private fun readyPage(page: Int) = TextPageState.Ready(PageJsonDto())

    @Test
    fun `同一页在途时拒绝第二次请求`() {
        val cache = TextPageCache()
        assertTrue(cache.begin(3, emptyMap()))
        assertFalse(cache.begin(3, emptyMap()))
        cache.finish(3)
        assertTrue("请求结束后必须能重取", cache.begin(3, emptyMap()))
    }

    @Test
    fun `已有内容时不再取，除非强制重试`() {
        val cache = TextPageCache()
        val pages = mapOf(5 to readyPage(5))
        assertFalse(cache.begin(5, pages))
        assertTrue(cache.begin(5, pages, force = true))
        // force 只绕过"已有内容"，不绕过在途去重
        assertFalse(cache.begin(5, pages, force = true))
    }

    @Test
    fun `取消留下的 Loading 残影不影响下一次请求`() {
        val cache = TextPageCache()
        // 模拟：请求被取消，finish 已调用，但 UI 里还留着 Loading
        cache.begin(7, emptyMap())
        cache.finish(7)
        val stale = mapOf(7 to TextPageState.Loading)
        assertTrue(cache.begin(7, stale))

        // dropLoading 只抹 Loading，Ready/Failed 不动
        assertEquals(emptyMap<Int, TextPageState>(), cache.dropLoading(stale, 7))
        val failed = mapOf(7 to TextPageState.Failed("x"))
        assertEquals(failed, cache.dropLoading(failed, 7))
    }

    @Test
    fun `缓存只保留离当前页最近的页`() {
        val cache = TextPageCache(capacity = 3)
        val pages = (1..10).associateWith { readyPage(it) }
        val pruned = cache.prune(pages, currentPage = 5)
        assertEquals(setOf(4, 5, 6), pruned.keys)
    }

    @Test
    fun `新增页面时按当前页裁剪`() {
        val cache = TextPageCache(capacity = 3)
        var pages: Map<Int, TextPageState> =
            mapOf(1 to readyPage(1), 2 to readyPage(2), 3 to readyPage(3))
        pages = cache.loading(pages, 10, currentPage = 10)
        assertEquals(setOf(2, 3, 10), pages.keys)

        pages = cache.ready(pages, 10, PageJsonDto(), currentPage = 10)
        assertTrue(pages[10] is TextPageState.Ready)

        pages = cache.failed(pages, 11, "boom", currentPage = 10)
        assertEquals(setOf(3, 10, 11), pages.keys)
        assertEquals(TextPageState.Failed("boom"), pages[11])
    }

    @Test
    fun `未超上限时不重建 map`() {
        val cache = TextPageCache(capacity = 12)
        val pages = mapOf(1 to readyPage(1))
        assertTrue(pages === cache.prune(pages, 1))
        assertNull(pages[2])
    }
}
