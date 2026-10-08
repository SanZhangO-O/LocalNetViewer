package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.reader.FitMode
import com.zqr.localnetviewer.ui.reader.ReaderTheme
import com.zqr.localnetviewer.ui.reader.ReadingDirection
import com.zqr.localnetviewer.ui.reader.ReaderViewModel
import kotlin.math.roundToInt

/**
 * 阅读设置面板（设计 6.3）：字号 / 行距 / 边距 / 段间距 / 主题 / 字体 + 漫画控制。
 *
 * 几何项在松手时提交（内部防抖由 ViewModel 300ms 完成），避免拖动过程中反复重新分页。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsSheet(
    state: ReaderViewModel.UiState,
    onSettingsChange: (AppSettings) -> Unit,
    onClearCache: () -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val settings = state.settings

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                // 横屏 / 小屏自适应：按可用高度的比例收，而不是固定 dp
                // （横屏可用高度不足 560dp 时固定上限会顶满甚至超出屏幕）
                .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.75f).dp)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp)
                .padding(bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("阅读设置", style = MaterialTheme.typography.titleMedium)

            Text("界面预设", style = MaterialTheme.typography.titleSmall)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                presetOptions.forEach { preset ->
                    FilterChip(
                        selected = settings.uiPreset == preset.id,
                        onClick = { onSettingsChange(settings.copy(uiPreset = preset.id)) },
                        label = { Text(preset.label) },
                    )
                }
            }
            Text(
                text = when (settings.uiPreset) {
                    AppSettings.PRESET_EINK -> "过渡直接落位，避免墨水屏多次全刷与残影"
                    AppSettings.PRESET_LCD -> "翻页跳转与工具栏显隐带动画，适合常规屏幕"
                    else -> "自行控制界面过渡动画开关"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (settings.uiPreset == AppSettings.PRESET_CUSTOM) {
                SwitchRow(
                    label = "界面过渡动画",
                    description = "翻页跳转 / 工具栏显隐 / 列表滚动是否带动画",
                    checked = settings.animatedTransitions,
                    onCheckedChange = {
                        onSettingsChange(settings.copy(animatedTransitions = it))
                    },
                )
            }

            LabeledSlider(
                label = "字号",
                value = settings.fontSizeSp.toFloat(),
                range = 12f..32f,
                valueText = { "${it.roundToInt()} sp" },
                onCommit = { onSettingsChange(settings.copy(fontSizeSp = it.roundToInt())) },
            )
            LabeledSlider(
                label = "行距",
                value = settings.lineHeight,
                range = 1.2f..2.4f,
                valueText = { String.format(java.util.Locale.US, "%.2f×", it) },
                onCommit = { onSettingsChange(settings.copy(lineHeight = it)) },
            )
            LabeledSlider(
                label = "左右边距",
                value = settings.marginHorizontalDp.toFloat(),
                range = 0f..48f,
                valueText = { "${it.roundToInt()} dp" },
                onCommit = { onSettingsChange(settings.copy(marginHorizontalDp = it.roundToInt())) },
            )
            LabeledSlider(
                label = "上下边距",
                value = settings.marginVerticalDp.toFloat(),
                range = 0f..32f,
                valueText = { "${it.roundToInt()} dp" },
                onCommit = { onSettingsChange(settings.copy(marginVerticalDp = it.roundToInt())) },
            )
            LabeledSlider(
                label = "段落间距",
                value = settings.paragraphSpacingSp.toFloat(),
                range = 0f..24f,
                valueText = { "${it.roundToInt()} sp" },
                onCommit = { onSettingsChange(settings.copy(paragraphSpacingSp = it.roundToInt())) },
            )

            Text("主题", style = MaterialTheme.typography.titleSmall)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                ReaderTheme.entries.forEach { theme ->
                    FilterChip(
                        selected = settings.theme == theme.id,
                        onClick = { onSettingsChange(settings.copy(theme = theme.id)) },
                        label = { Text(themeLabel(theme)) },
                    )
                }
            }

            if (state.fonts.isNotEmpty()) {
                Text("字体", style = MaterialTheme.typography.titleSmall)
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    FilterChip(
                        selected = settings.fontFamily == "auto",
                        onClick = { onSettingsChange(settings.copy(fontFamily = "auto")) },
                        label = { Text("服务器默认") },
                    )
                    state.fonts.forEach { font ->
                        FilterChip(
                            selected = settings.fontFamily == font.id,
                            onClick = { onSettingsChange(settings.copy(fontFamily = font.id)) },
                            label = { Text(font.name.ifEmpty { font.id }) },
                        )
                    }
                }
            }

            if (!state.isFixed) {
                Text("页面模式", style = MaterialTheme.typography.titleSmall)
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    FilterChip(
                        selected = settings.pageRenderMode == AppSettings.RENDER_SERVER,
                        onClick = {
                            onSettingsChange(settings.copy(pageRenderMode = AppSettings.RENDER_SERVER))
                        },
                        label = { Text("服务器图片") },
                    )
                    FilterChip(
                        selected = settings.pageRenderMode == AppSettings.RENDER_TEXT,
                        onClick = {
                            onSettingsChange(settings.copy(pageRenderMode = AppSettings.RENDER_TEXT))
                        },
                        label = { Text("文字直发") },
                    )
                }
                Text(
                    text = when {
                        settings.pageRenderMode != AppSettings.RENDER_TEXT ->
                            "服务器排版并渲染成图片下发：观感最稳、流量较大。"
                        !state.textSupported ->
                            "当前服务器不支持文字直发（旧版服务器），将自动使用服务器图片。"
                        else ->
                            "服务器只下发排版数据，文字由手机绘制：更清晰、省流量；" +
                                "首次使用会自动下载服务器字体。漫画 / PDF 不受影响。"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            if (state.isFixed) {
                Text("漫画 / 固定版式", style = MaterialTheme.typography.titleSmall)

                Text("阅读方向", style = MaterialTheme.typography.labelLarge)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ReadingDirection.entries.forEach { direction ->
                        FilterChip(
                            selected = settings.readingDirection == direction.id,
                            onClick = {
                                onSettingsChange(settings.copy(readingDirection = direction.id))
                            },
                            label = { Text(direction.label) },
                        )
                    }
                }

                if (!state.strip) {
                    Text("适应模式", style = MaterialTheme.typography.labelLarge)
                    val currentFit = FitMode.of(settings.fitMode)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FitMode.entries.forEach { mode ->
                            FilterChip(
                                selected = currentFit == mode,
                                onClick = { onSettingsChange(settings.copy(fitMode = mode.id)) },
                                label = { Text(mode.label) },
                            )
                        }
                    }
                }

                SwitchRow(
                    label = "原图模式",
                    description = "直通容器内原始图片字节（保真、流量更大）",
                    checked = settings.originalQuality,
                    onCheckedChange = {
                        onSettingsChange(settings.copy(originalQuality = it))
                    },
                )
                SwitchRow(
                    label = "条漫模式",
                    description = "长图按视口高度切片，纵向连续滚动",
                    checked = settings.stripMode,
                    onCheckedChange = { onSettingsChange(settings.copy(stripMode = it)) },
                )
                if (!state.strip) {
                    SwitchRow(
                        label = "双页跨页",
                        description = "相邻两页横向拼接展示（开启后每屏翻两页）",
                        checked = settings.pairPages,
                        onCheckedChange = { onSettingsChange(settings.copy(pairPages = it)) },
                    )
                }
            }

            SwitchRow(
                label = "阅读时保持常亮",
                description = null,
                checked = settings.keepScreenOn,
                onCheckedChange = { onSettingsChange(settings.copy(keepScreenOn = it)) },
            )

            Button(onClick = onClearCache, modifier = Modifier.fillMaxWidth()) {
                Text("清理页面缓存")
            }
            Text(
                text = "清理后会重新从服务器取页；服务器端缓存不受影响。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun LabeledSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    valueText: (Float) -> String,
    onCommit: (Float) -> Unit,
) {
    var local by remember(value) { mutableFloatStateOf(value) }
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = MaterialTheme.typography.labelLarge)
            Spacer(modifier = Modifier.weight(1f))
            Text(valueText(local), style = MaterialTheme.typography.labelMedium)
        }
        Slider(
            value = local,
            onValueChange = { local = it },
            onValueChangeFinished = { onCommit(local) },
            valueRange = range,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun SwitchRow(
    label: String,
    description: String?,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.labelLarge)
            if (description != null) {
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

private data class PresetOption(val id: String, val label: String)

private val presetOptions = listOf(
    PresetOption(AppSettings.PRESET_EINK, "墨水屏"),
    PresetOption(AppSettings.PRESET_LCD, "常规屏"),
    PresetOption(AppSettings.PRESET_CUSTOM, "自定义"),
)

private fun themeLabel(theme: ReaderTheme): String = when (theme) {
    ReaderTheme.WHITE -> "白"
    ReaderTheme.PAPER -> "纸"
    ReaderTheme.SEPIA -> "棕"
    ReaderTheme.GREEN -> "绿"
    ReaderTheme.DARK -> "黑"
}
