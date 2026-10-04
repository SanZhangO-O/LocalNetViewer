package com.zqr.localnetviewer

import androidx.compose.ui.layout.ContentScale
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.reader.FitMode
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis
import com.zqr.localnetviewer.ui.reader.contentScaleFor
import com.zqr.localnetviewer.ui.reader.innerScrollFor
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * 漫画（固定版式）默认"铺满屏幕"（fitMode = "screen"）：
 * 整页自动拉伸占满整块屏幕、不留黑边；不做页内滚动。
 *
 * 该测试把默认值锁住——改默认适应模式前必须显式更新这里的预期。
 */
class ReaderFitModeDefaultTest {

    @Test
    fun `默认适应模式是铺满屏幕`() {
        assertEquals("screen", AppSettings.DEFAULT.fitMode)
        assertEquals(FitMode.SCREEN, FitMode.of(AppSettings.DEFAULT.fitMode))
    }

    @Test
    fun `铺满屏幕模式下拉伸占满且无页内滚动`() {
        val settings = AppSettings.DEFAULT
        assertEquals(ContentScale.FillBounds, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }

    @Test
    fun `整页模式仍为等比完整显示`() {
        val settings = AppSettings.DEFAULT.copy(fitMode = "page")
        assertEquals(ContentScale.Fit, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }

    @Test
    fun `未知适应模式兜底为铺满屏幕`() {
        assertEquals(FitMode.SCREEN, FitMode.of("nonsense"))
        val settings = AppSettings.DEFAULT.copy(fitMode = "nonsense")
        assertEquals(ContentScale.FillBounds, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }
}
