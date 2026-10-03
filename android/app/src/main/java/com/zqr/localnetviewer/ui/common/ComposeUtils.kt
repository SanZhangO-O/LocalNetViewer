package com.zqr.localnetviewer.ui.common

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.res.Configuration
import android.os.Build
import android.view.WindowManager
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp

/** 像素边距（服务器排版参数全部用物理像素，设计 4.6）。 */
data class InsetsPx(
    val left: Int = 0,
    val top: Int = 0,
    val right: Int = 0,
    val bottom: Int = 0,
)

/**
 * 窗口全尺寸（像素）。
 *
 * 设计 4.6：viewport 是**窗口全尺寸**，不随系统栏显隐变化；多余区域由客户端显示主题背景。
 */
@Composable
fun rememberWindowSizePx(): IntSize {
    val view = LocalView.current
    val configuration = LocalConfiguration.current
    val density = LocalDensity.current.density
    return remember(view, configuration) {
        val bounds = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            view.context.getSystemService(WindowManager::class.java)
                ?.currentWindowMetrics
                ?.bounds
        } else {
            null
        }
        if (bounds != null) {
            IntSize(bounds.width(), bounds.height())
        } else {
            fallbackSize(configuration, density)
        }
    }
}

private fun fallbackSize(configuration: Configuration, density: Float): IntSize {
    val w = (configuration.screenWidthDp * density).toInt()
    val h = (configuration.screenHeightDp * density).toInt()
    return IntSize(w.coerceAtLeast(320), h.coerceAtLeast(320))
}

/**
 * 常态 WindowInsets（状态栏 / 刘海 / 手势条），取"运行期最大值"。
 *
 * 沉浸模式下系统栏消失、insets 归零；若直接用当前值重排，会出现"一进沉浸就重新分页"。
 * 这里保留历史最大值，保证切换沉浸模式**不触发重新布局**（设计 4.6 / 6.4）。
 *
 * 实现说明：用普通可变持有对象在组合期取最大值（幂等、只增不减），而不是把结果写回
 * `mutableStateOf` —— 后者会在组合后多触发一轮重组。`current` 本身来自 Compose 的
 * WindowInsets 状态，它变化时必然会重组，因此这里不需要额外的可观察状态。
 */
private class InsetsMaxHolder {
    var value = InsetsPx()
}

@Composable
fun rememberStableInsets(): InsetsPx {
    val density = LocalDensity.current
    val layoutDirection = LocalLayoutDirection.current
    val systemBars = WindowInsets.systemBars
    val cutout = WindowInsets.displayCutout

    val current = InsetsPx(
        left = maxOf(
            systemBars.getLeft(density, layoutDirection),
            cutout.getLeft(density, layoutDirection),
        ),
        top = maxOf(
            systemBars.getTop(density),
            cutout.getTop(density),
        ),
        right = maxOf(
            systemBars.getRight(density, layoutDirection),
            cutout.getRight(density, layoutDirection),
        ),
        bottom = maxOf(
            systemBars.getBottom(density),
            cutout.getBottom(density),
        ),
    )

    val holder = remember { InsetsMaxHolder() }
    val merged = InsetsPx(
        left = maxOf(holder.value.left, current.left),
        top = maxOf(holder.value.top, current.top),
        right = maxOf(holder.value.right, current.right),
        bottom = maxOf(holder.value.bottom, current.bottom),
    )
    holder.value = merged
    return merged
}

/** 从 Context 找 Activity（沉浸模式控制需要）。 */
fun Context.findActivity(): Activity? {
    var context: Context? = this
    while (context is ContextWrapper) {
        if (context is Activity) return context
        context = context.baseContext
    }
    return null
}

@Composable
fun LoadingView(modifier: Modifier = Modifier, text: String = "加载中…") {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator()
            Text(
                text = text,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(top = 12.dp),
            )
        }
    }
}

@Composable
fun ErrorView(
    message: String,
    modifier: Modifier = Modifier,
    onRetry: (() -> Unit)? = null,
    onSecondary: (() -> Unit)? = null,
    secondaryLabel: String = "返回",
) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.padding(24.dp),
        ) {
            Text(text = message, style = MaterialTheme.typography.bodyLarge)
            if (onRetry != null) {
                Button(onClick = onRetry, modifier = Modifier.padding(top = 16.dp)) {
                    Text("重试")
                }
            }
            if (onSecondary != null) {
                Button(onClick = onSecondary, modifier = Modifier.padding(top = 8.dp)) {
                    Text(secondaryLabel)
                }
            }
        }
    }
}

@Composable
fun EmptyView(text: String, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text = text, style = MaterialTheme.typography.bodyMedium)
    }
}

/** 人类可读的文件大小（列表展示用）。 */
fun formatSize(bytes: Long?): String {
    val value = bytes ?: return ""
    if (value <= 0) return ""
    val units = listOf("B", "KB", "MB", "GB", "TB")
    var size = value.toDouble()
    var index = 0
    while (size >= 1024 && index < units.lastIndex) {
        size /= 1024.0
        index++
    }
    return if (index == 0) "${value}B" else String.format(java.util.Locale.US, "%.1f%s", size, units[index])
}
