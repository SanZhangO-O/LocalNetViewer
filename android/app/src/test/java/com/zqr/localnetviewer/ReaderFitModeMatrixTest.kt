package com.zqr.localnetviewer

import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.IntSize
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.common.InsetsPx
import com.zqr.localnetviewer.ui.reader.FillScreenScale
import com.zqr.localnetviewer.ui.reader.FitMode
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis
import com.zqr.localnetviewer.ui.reader.contentScaleFor
import com.zqr.localnetviewer.ui.reader.effectiveMode
import com.zqr.localnetviewer.ui.reader.innerScrollFor
import com.zqr.localnetviewer.ui.reader.needsInnerScroll
import com.zqr.localnetviewer.ui.reader.profileSignature
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 适应模式（固定版式）的显示策略矩阵。
 *
 * 这些组合直接决定漫画/PDF 的观感与手势仲裁（设计 6.4），且默认值已改为"铺满屏幕"，
 * 所以把整张表锁在测试里：改默认或改映射必须显式改这里。
 */
class ReaderFitModeMatrixTest {

    private fun settings(fit: String) = AppSettings.DEFAULT.copy(fitMode = fit)

    @Test
    fun `固定版式四种模式的显示策略`() {
        val expected = mapOf(
            "screen" to (FillScreenScale to InnerScrollAxis.NONE),
            "page" to (ContentScale.Fit to InnerScrollAxis.NONE),
            "width" to (ContentScale.FillWidth to InnerScrollAxis.VERTICAL),
            "height" to (ContentScale.FillHeight to InnerScrollAxis.HORIZONTAL),
        )
        expected.forEach { (id, pair) ->
            val s = settings(id)
            assertEquals("contentScale($id)", pair.first, contentScaleFor(s, isFixed = true))
            assertEquals("innerScroll($id)", pair.second, innerScrollFor(s, isFixed = true))
        }
    }

    @Test
    fun `文本书忽略适应模式`() {
        for (id in listOf("screen", "page", "width", "height", "nonsense")) {
            val s = settings(id)
            assertEquals(ContentScale.Fit, contentScaleFor(s, isFixed = false))
            assertEquals(InnerScrollAxis.NONE, innerScrollFor(s, isFixed = false))
            assertFalse(needsInnerScroll(s, isFixed = false))
        }
    }

    @Test
    fun `只有 width 与 height 需要页内滚动`() {
        assertFalse(needsInnerScroll(settings("screen"), isFixed = true))
        assertFalse(needsInnerScroll(settings("page"), isFixed = true))
        assertTrue(needsInnerScroll(settings("width"), isFixed = true))
        assertTrue(needsInnerScroll(settings("height"), isFixed = true))
    }

    @Test
    fun `模式 id 与枚举一一对应且兜底为铺满屏幕`() {
        FitMode.entries.forEach { assertEquals(it, FitMode.of(it.id)) }
        assertEquals(FitMode.SCREEN, FitMode.of(""))
        assertEquals(FitMode.SCREEN, FitMode.of("nonsense"))
        // 老版本存的是 "page"：必须继续解析成整页，而不是被兜底改掉
        assertEquals(FitMode.PAGE, FitMode.of("page"))
    }

    @Test
    fun `适应模式不参与分页签名`() {
        val a = settings("page")
        val b = settings("screen")
        val size = IntSize(1080, 1920)
        val insets = InsetsPx(0, 0, 0, 0)
        // 只改适应模式不应触发重新 /layout（设计 6.4：只改客户端显示策略）
        assertEquals(
            profileSignature(size, a, 2.0f, insets, "fixed"),
            profileSignature(size, b, 2.0f, insets, "fixed"),
        )
        // 但条漫开关会改变 mode，必须触发
        assertNotEquals(
            profileSignature(size, a, 2.0f, insets, "fixed"),
            profileSignature(size, a.copy(stripMode = true), 2.0f, insets, "fixed"),
        )
    }

    @Test
    fun `effectiveMode 规则`() {
        val s = AppSettings.DEFAULT
        assertEquals("flow", effectiveMode(s, "flow"))
        assertEquals("fixed", effectiveMode(s, "fixed"))
        assertEquals("strip", effectiveMode(s.copy(stripMode = true), "fixed"))
        // 文本书即使开了条漫也仍是 flow
        assertEquals("flow", effectiveMode(s.copy(stripMode = true), "flow"))
    }
}
