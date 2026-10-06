package com.zqr.localnetviewer

import androidx.compose.ui.geometry.Size
import androidx.compose.ui.layout.ContentScale
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.reader.FitMode
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis
import com.zqr.localnetviewer.ui.reader.contentScaleFor
import com.zqr.localnetviewer.ui.reader.innerScrollFor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 漫画（固定版式）默认"铺满屏幕"（fitMode = "screen"）：
 * 等比缩放**完整显示**整页——先保证图片内容不被裁切，再尽量占满屏幕
 * （`Fit` 语义），比例不符时只留黑边；不做页内滚动。
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
    fun `铺满屏幕模式下等比完整显示且无页内滚动`() {
        val settings = AppSettings.DEFAULT
        assertEquals(ContentScale.Fit, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }

    @Test
    fun `铺满屏幕绝不裁边——比例接近与差距大都完整显示`() {
        val src = Size(1000f, 1500f)
        val scale = contentScaleFor(AppSettings.DEFAULT, isFixed = true)
        // 比例接近（旧实现会按 Crop 裁边 ≤8% 铺满）与比例差距大（竖屏手机 vs 横向跨页）
        // 两种视口都必须完整显示：等比缩放后图片不超出视口任一方向。
        listOf(Size(1040f, 1620f), Size(1080f, 2400f)).forEach { viewport ->
            val factor = scale.computeScaleFactor(src, viewport)
            assertEquals(ContentScale.Fit.computeScaleFactor(src, viewport), factor)
            assertTrue(factor.scaleX * src.width <= viewport.width + 0.01f)
            assertTrue(factor.scaleY * src.height <= viewport.height + 0.01f)
        }
    }

    @Test
    fun `未知适应模式兜底为铺满屏幕`() {
        assertEquals(FitMode.SCREEN, FitMode.of("nonsense"))
        val settings = AppSettings.DEFAULT.copy(fitMode = "nonsense")
        assertEquals(ContentScale.Fit, contentScaleFor(settings, isFixed = true))
        assertEquals(InnerScrollAxis.NONE, innerScrollFor(settings, isFixed = true))
    }
}
