package com.zqr.localnetviewer

import com.zqr.localnetviewer.data.prefs.AppSettings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 界面预设默认"墨水屏"（eink）：所有界面过渡（页面间导航、跳页滚动、工具栏显隐）
 * 直接落位——渐变/滑动动画在墨水屏上表现为多次灰阶全刷与残影。
 *
 * 该测试把默认值锁住——改默认预设前必须显式更新这里的预期。
 */
class UiPresetDefaultTest {

    @Test
    fun `默认预设是墨水屏且无界面过渡动画`() {
        assertEquals(AppSettings.PRESET_EINK, AppSettings.DEFAULT.uiPreset)
        assertFalse(AppSettings.DEFAULT.animatedTransitions)
        assertFalse(AppSettings.DEFAULT.animateTransitions)
    }

    @Test
    fun `常规屏预设开动画`() {
        val settings = AppSettings.DEFAULT.copy(uiPreset = AppSettings.PRESET_LCD)
        assertTrue(settings.animateTransitions)
    }

    @Test
    fun `自定义预设跟随动画开关`() {
        val off = AppSettings.DEFAULT.copy(uiPreset = AppSettings.PRESET_CUSTOM)
        assertFalse(off.animateTransitions)
        val on = off.copy(animatedTransitions = true)
        assertTrue(on.animateTransitions)
    }

    @Test
    fun `未知预设按自定义语义兜底到开关`() {
        val settings = AppSettings.DEFAULT.copy(uiPreset = "nonsense")
        assertFalse(settings.animateTransitions)
        assertTrue(settings.copy(animatedTransitions = true).animateTransitions)
    }
}
