package com.zqr.localnetviewer

import com.zqr.localnetviewer.data.prefs.ReadingProgress
import com.zqr.localnetviewer.data.prefs.RecentBook
import com.zqr.localnetviewer.ui.browser.recentInPath
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

/** "继续阅读"按当前目录过滤（前缀边界是这类过滤最容易写错的地方）。 */
class RecentInPathTest {

    private fun book(id: String, vpath: String) = RecentBook(
        bookId = id,
        progress = ReadingProgress(vpath = vpath, ratio = 0.5f),
    )

    private val shelf = listOf(
        book("a", "/lib/小说/三体.txt"),
        book("b", "/lib/小说/活着.txt"),
        book("c", "/lib/技术/代码大全.pdf"),
        book("d", "/lib/小说2/番外.txt"),
        book("e", "/lib/根目录的书.txt"),
    )

    @Test
    fun `根目录显示全部最近阅读`() {
        assertEquals(shelf, recentInPath(shelf, "/"))
    }

    @Test
    fun `子目录只显示该子树的书`() {
        assertEquals(listOf("a", "b"), recentInPath(shelf, "/lib/小说").map { it.bookId })
    }

    @Test
    fun `前缀相同的兄弟目录不算命中`() {
        // "/lib/小说2/..." 不能因为以 "/lib/小说" 开头就被算进来
        val ids = recentInPath(shelf, "/lib/小说").map { it.bookId }
        assertFalse("小说2 不是 小说 的子目录", ids.contains("d"))
        assertEquals(listOf("a", "b"), ids)
    }

    @Test
    fun `书库根目录显示整棵子树（含直接放在根下的书）`() {
        assertEquals(
            shelf.map { it.bookId },
            recentInPath(shelf, "/lib").map { it.bookId },
        )
    }

    @Test
    fun `空目录返回空列表`() {
        assertEquals(emptyList<RecentBook>(), recentInPath(shelf, "/lib/漫画"))
    }
}
