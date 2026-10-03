package com.zqr.localnetviewer.ui.theme

import androidx.compose.ui.graphics.Color

/**
 * 应用配色（设计 6.7「视觉身份」）。
 *
 * 取"墨与纸"的意象：主色用偏冷的墨青（而不是模板紫），中性色带一点暖调，
 * 与阅读页的五套阅读主题（白/纸/棕/绿/黑）在气质上一致。
 * 阅读正文的颜色**不来自这里** —— 那是服务器端主题（render/raster.py THEMES），
 * 这里只管界面外壳。
 */

// ---- 亮色
val InkPrimary = Color(0xFF2F5D62)          // 墨青
val InkOnPrimary = Color(0xFFFFFFFF)
val InkPrimaryContainer = Color(0xFFB6DDE1)
val InkOnPrimaryContainer = Color(0xFF06282C)

val ClaySecondary = Color(0xFF7A5C46)        // 陶土棕
val ClayOnSecondary = Color(0xFFFFFFFF)
val ClaySecondaryContainer = Color(0xFFF2DECF)
val ClayOnSecondaryContainer = Color(0xFF2C1B10)

val SageTertiary = Color(0xFF4F6350)         // 苔绿
val SageOnTertiary = Color(0xFFFFFFFF)

val PaperBackground = Color(0xFFFAF8F5)      // 暖白纸面
val PaperOnBackground = Color(0xFF1B1C1C)
val PaperSurface = Color(0xFFFAF8F5)
val PaperOnSurface = Color(0xFF1B1C1C)
val PaperSurfaceVariant = Color(0xFFDCE4E4)
val PaperOnSurfaceVariant = Color(0xFF3F4949)
val PaperOutline = Color(0xFF6F7979)
val InkError = Color(0xFFBA1A1A)
val InkOnError = Color(0xFFFFFFFF)

// ---- 暗色
val InkPrimaryDark = Color(0xFF9ACBD0)
val InkOnPrimaryDark = Color(0xFF00363B)
val InkPrimaryContainerDark = Color(0xFF15494E)
val InkOnPrimaryContainerDark = Color(0xFFB6DDE1)

val ClaySecondaryDark = Color(0xFFE0C2AE)
val ClayOnSecondaryDark = Color(0xFF432C1B)
val ClaySecondaryContainerDark = Color(0xFF5C4230)
val ClayOnSecondaryContainerDark = Color(0xFFF2DECF)

val SageTertiaryDark = Color(0xFFB6CCB7)
val SageOnTertiaryDark = Color(0xFF213523)

val NightBackground = Color(0xFF121414)      // 近黑（与阅读页 dark 主题一致）
val NightOnBackground = Color(0xFFE2E2E2)
val NightSurface = Color(0xFF121414)
val NightOnSurface = Color(0xFFE2E2E2)
val NightSurfaceVariant = Color(0xFF3F4949)
val NightOnSurfaceVariant = Color(0xFFBEC8C8)
val NightOutline = Color(0xFF899393)
val NightError = Color(0xFFFFB4AB)
val NightOnError = Color(0xFF690005)
