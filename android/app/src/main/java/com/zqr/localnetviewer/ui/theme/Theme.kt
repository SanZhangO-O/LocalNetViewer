package com.zqr.localnetviewer.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext

/**
 * 应用主题（设计 6.7）。
 *
 * 默认使用自定义配色，而不是 Material 模板紫：这是一个阅读类应用，
 * 界面外壳应当安静、不抢正文；`dynamicColor` 默认关闭（取系统壁纸取色会让
 * "墨与纸"的观感随机漂移，也让截图/文档难以复现）。
 */
private val DarkColorScheme = darkColorScheme(
    primary = InkPrimaryDark,
    onPrimary = InkOnPrimaryDark,
    primaryContainer = InkPrimaryContainerDark,
    onPrimaryContainer = InkOnPrimaryContainerDark,
    secondary = ClaySecondaryDark,
    onSecondary = ClayOnSecondaryDark,
    secondaryContainer = ClaySecondaryContainerDark,
    onSecondaryContainer = ClayOnSecondaryContainerDark,
    tertiary = SageTertiaryDark,
    onTertiary = SageOnTertiaryDark,
    background = NightBackground,
    onBackground = NightOnBackground,
    surface = NightSurface,
    onSurface = NightOnSurface,
    surfaceVariant = NightSurfaceVariant,
    onSurfaceVariant = NightOnSurfaceVariant,
    outline = NightOutline,
    error = NightError,
    onError = NightOnError,
)

private val LightColorScheme = lightColorScheme(
    primary = InkPrimary,
    onPrimary = InkOnPrimary,
    primaryContainer = InkPrimaryContainer,
    onPrimaryContainer = InkOnPrimaryContainer,
    secondary = ClaySecondary,
    onSecondary = ClayOnSecondary,
    secondaryContainer = ClaySecondaryContainer,
    onSecondaryContainer = ClayOnSecondaryContainer,
    tertiary = SageTertiary,
    onTertiary = SageOnTertiary,
    background = PaperBackground,
    onBackground = PaperOnBackground,
    surface = PaperSurface,
    onSurface = PaperOnSurface,
    surfaceVariant = PaperSurfaceVariant,
    onSurfaceVariant = PaperOnSurfaceVariant,
    outline = PaperOutline,
    error = InkError,
    onError = InkOnError,
)

@Composable
fun LocalNetViewerTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    // 动态取色（Android 12+）：默认关闭，见文件头说明
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }

        darkTheme -> DarkColorScheme
        else -> LightColorScheme
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = Typography,
        content = content,
    )
}
