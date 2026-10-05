package com.zqr.localnetviewer

import androidx.compose.ui.geometry.Size
import androidx.compose.ui.layout.ContentScale
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.reader.FillScreenScale
import com.zqr.localnetviewer.ui.reader.FitMode
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis
import com.zqr.localnetviewer.ui.reader.contentScaleFor
import com.zqr.localnetviewer.ui.reader.innerScrollFor
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * 漫画（固定版式）默认"铺满屏幕"（fitMode = "screen"）：
 * 比例接近时等比放大占满整块屏幕（保持原比例、仅轻微裁边）、不留黑边；
 * 比例差距大时退化为完整显示，避免只看到画面中心一小部分。不做页内滚动。
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
    fun `铺满屏幕模式下等比占满且无页内滚动`() {
        val settings = AppSettings.DEFAULT
        assertEquals(FillScreenScale, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }

    @Test
    fun `比例接近时裁边铺满、差距大时完整显示`() {
        val src = Size(1000f, 1500f)
        // 视口比例与图片接近（裁边 < 8%）：覆盖铺满
        val near = Size(1040f, 1620f)
        assertEquals(
            ContentScale.Crop.computeScaleFactor(src, near),
            FillScreenScale.computeScaleFactor(src, near),
        )
        // 视口比图片窄得多（竖屏手机 vs 横向跨页/单页场景）：退化为完整显示
        val far = Size(1080f, 2400f)
        assertEquals(
            ContentScale.Fit.computeScaleFactor(src, far),
            FillScreenScale.computeScaleFactor(src, far),
        )
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
        assertEquals(FillScreenScale, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }
}
