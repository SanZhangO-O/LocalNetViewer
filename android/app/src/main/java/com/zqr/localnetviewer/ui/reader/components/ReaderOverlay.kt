package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.FormatSize
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zqr.localnetviewer.ui.reader.ReaderViewModel
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * 阅读器工具栏（设计 6.3）：顶栏（返回 / 搜索 / 目录 / 设置）+ 底栏（进度条 + 页码章节）。
 *
 * 页码与章节名由客户端根据 layout 响应的章节表 + 当前页序号自行计算，无需逐页请求。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ReaderOverlay(
    state: ReaderViewModel.UiState,
    onBack: () -> Unit,
    onOpenToc: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenSearch: () -> Unit,
    onSeek: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    // 墨水屏预设（默认）不用 AnimatedVisibility 淡入淡出：渐变动画在墨水屏上
    // 表现为多次灰阶刷新 + 残影，直接显隐只需一次刷新；lcd/custom 开动画时保留渐变。
    val content: @Composable () -> Unit = {
        Column(modifier = modifier.fillMaxSize()) {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            text = state.title.ifEmpty { "阅读" },
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = MaterialTheme.typography.titleMedium,
                        )
                        if (state.chapterTitle.isNotEmpty()) {
                            Text(
                                text = state.chapterTitle,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = MaterialTheme.typography.labelMedium,
                            )
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                actions = {
                    IconButton(onClick = onOpenSearch) {
                        Icon(Icons.Default.Search, contentDescription = "搜索")
                    }
                    IconButton(onClick = onOpenToc) {
                        Icon(Icons.AutoMirrored.Filled.List, contentDescription = "目录")
                    }
                    IconButton(onClick = onOpenSettings) {
                        Icon(Icons.Default.FormatSize, contentDescription = "阅读设置")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.92f),
                ),
            )

            Spacer(modifier = Modifier.weight(1f))

            Surface(tonalElevation = 3.dp) {
                // 避让手势条 / 导航栏：页面是边到边的，不避让的话 Slider 与页码
                // 会被手势条盖住（视觉与触摸都受影响）
                Column(
                    modifier = Modifier
                        .windowInsetsPadding(WindowInsets.navigationBars)
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = state.pageLabel,
                            style = MaterialTheme.typography.labelLarge,
                        )
                        Spacer(modifier = Modifier.weight(1f))
                        if (state.preparing) {
                            Text("排版中…", style = MaterialTheme.typography.labelSmall)
                        }
                    }
                    var sliderValue by remember(state.currentPage) {
                        mutableFloatStateOf(state.currentPage.toFloat())
                    }
                    Slider(
                        value = sliderValue,
                        onValueChange = { sliderValue = it },
                        onValueChangeFinished = { onSeek(sliderValue.roundToInt()) },
                        valueRange = 1f..max(1, state.totalPages).toFloat(),
                        enabled = state.totalPages > 1,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        }
    }
    if (state.settings.animateTransitions) {
        // modifier 只由内层 content 的 Column 应用，这里不再重复传，避免叠加
        AnimatedVisibility(
            visible = state.overlayVisible,
            enter = fadeIn(),
            exit = fadeOut(),
        ) { content() }
    } else if (state.overlayVisible) {
        content()
    }
}
