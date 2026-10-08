package com.zqr.localnetviewer.ui.reader.components

import android.graphics.Typeface
import android.util.Log
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.LineHeightStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.dto.FontInfoDto
import com.zqr.localnetviewer.data.net.dto.PageBlockDto
import com.zqr.localnetviewer.data.net.dto.PageJsonDto
import com.zqr.localnetviewer.ui.reader.ReaderTheme
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * 文字直发模式的单页绘制（设计 3.2 / 5.3 JSON 模式）。
 *
 * 关键约定：**分页边界由服务器决定**——服务器按行下发（`lines`）并给出块的纵向
 * 坐标，客户端逐行绘制、不做重排，因此 JSON 模式与 PNG 模式页码一致。
 *
 * - 坐标单位是"排版视口物理像素"（客户端上报的窗口尺寸），这里按实际可用区域
 *   等比映射（正常情况下就是 1:1，只有布局刚变化时才短暂不等）；
 *   块的 y 相对**内容区**（不含 margin），绘制时必须叠加 `margin.top/left`；
 * - 字体优先用服务器字体文件（[FontStore]）；尚未下载完 / 下载失败时用系统字体，
 *   并对超出可用宽度的行做 ≤10% 横向压缩，仍超出则按设计截断并记录警告；
 * - 图片块严格按服务器给的 `display_w/display_h`（居中）布局，不按原图比例自行换算。
 */
@Composable
internal fun TextPageContent(
    page: PageJsonDto,
    theme: ReaderTheme,
    fonts: List<FontInfoDto>,
    modifier: Modifier = Modifier,
) {
    val viewport = page.viewport
    val margin = page.margin
    if (viewport == null || viewport.w <= 0 || viewport.h <= 0 || margin == null) {
        Box(modifier)
        return
    }
    val context = LocalContext.current
    val app = remember(context) { App.from(context) }
    val textMeasurer = rememberTextMeasurer()
    val fontSpec = page.font
    val lineRatio = (fontSpec?.lineHeight ?: 1.6f).coerceAtLeast(0.5f)
    val letterSpacingPx = fontSpec?.letterSpacingPx ?: 0f
    val baseSize = fontSpec?.sizePx ?: 0
    val fontFamily = rememberServerFontFamily(fontSpec?.family ?: "auto", fonts)
    val fg = Color(theme.foreground)
    val muted = blendSrgb(fg, Color(theme.background), 0.45f)

    BoxWithConstraints(modifier) {
        val density = LocalDensity.current
        val viewW = viewport.w.toFloat()
        val viewH = viewport.h.toFloat()
        // 等比铺满可用区域：viewport 由客户端按当前窗口上报，稳定态下比例因子 = 1
        val scale = min(constraints.maxWidth / viewW, constraints.maxHeight / viewH)
            .takeIf { it > 0f } ?: 1f
        val originX = (constraints.maxWidth - viewW * scale) / 2f
        val originY = (constraints.maxHeight - viewH * scale) / 2f
        val marginLeft = margin.left.toFloat()
        val marginTop = margin.top.toFloat()
        val availW = (viewW - marginLeft - margin.right).coerceAtLeast(1f)

        Canvas(Modifier.fillMaxSize()) {
            page.blocks.forEach { block ->
                when (block.type) {
                    "paragraph", "heading" -> {
                        drawTextBlock(
                            block = block,
                            textMeasurer = textMeasurer,
                            fontFamily = fontFamily,
                            baseSize = baseSize,
                            lineRatio = lineRatio,
                            letterSpacingPx = letterSpacingPx,
                            fg = fg,
                            marginLeft = marginLeft,
                            marginTop = marginTop,
                            scale = scale,
                            originX = originX,
                            originY = originY,
                            availW = availW,
                        )
                    }

                    "separator" -> {
                        // 与服务器一致：位于块纵向中点，x 取内容区 8%–92%
                        val y = originY + (marginTop + block.y + block.height / 2f) * scale
                        drawLine(
                            color = muted,
                            start = Offset(
                                originX + (marginLeft + availW * 0.08f) * scale,
                                y,
                            ),
                            end = Offset(
                                originX + (marginLeft + availW * 0.92f) * scale,
                                y,
                            ),
                            strokeWidth = max(1f, scale),
                        )
                    }
                }
            }
        }

        // 图片块走 Coil（与 PNG 模式同一条资源通道），按服务器给的显示尺寸居中
        page.blocks.forEach { block ->
            if (block.type != "image") return@forEach
            val widthPx = (block.displayW * scale).coerceAtLeast(1f)
            val heightPx = (block.displayH * scale).coerceAtLeast(1f)
            val x = originX + (marginLeft + max(0f, (availW - block.displayW) / 2f)) * scale
            val y = originY + (marginTop + block.y) * scale
            val boxModifier = Modifier
                .offset { IntOffset(x.roundToInt(), y.roundToInt()) }
                .size(
                    width = with(density) { widthPx.toDp() },
                    height = with(density) { heightPx.toDp() },
                )
            val asset = block.asset
            if (asset.isNullOrBlank()) {
                ImagePlaceholder(boxModifier, muted)
            } else {
                var imageFailed by remember(asset) { mutableStateOf(false) }
                Box(boxModifier, contentAlignment = Alignment.Center) {
                    if (imageFailed) {
                        // 与服务器一致：失败画占位框，不留空白
                        ImagePlaceholder(Modifier.fillMaxSize(), muted)
                    } else {
                        AsyncImage(
                            model = app.apiClient.absoluteUrl(asset),
                            contentDescription = null,
                            contentScale = ContentScale.FillBounds,
                            modifier = Modifier.fillMaxSize(),
                            onError = { imageFailed = true },
                        )
                    }
                }
            }
        }
    }
}

/** 图片不可用占位（对齐服务器 `_draw_missing_image` 的视觉语义）。 */
@Composable
private fun ImagePlaceholder(modifier: Modifier, color: Color) {
    Box(modifier = modifier.border(1.dp, color), contentAlignment = Alignment.Center) {
        Text(
            text = "图片无法显示",
            style = MaterialTheme.typography.labelSmall,
            color = color,
        )
    }
}

/**
 * 绘制一个文本块：`lines` 已由服务器折好，按 `line_height × font_size` 逐行推进；
 * 首行纵坐标 = 内容区顶部 + 服务器给的 `block.y`。
 */
private fun DrawScope.drawTextBlock(
    block: PageBlockDto,
    textMeasurer: TextMeasurer,
    fontFamily: FontFamily?,
    baseSize: Int,
    lineRatio: Float,
    letterSpacingPx: Float,
    fg: Color,
    marginLeft: Float,
    marginTop: Float,
    scale: Float,
    originX: Float,
    originY: Float,
    availW: Float,
) {
    val sizePx = (if (block.fontSize > 0) block.fontSize else baseSize).toFloat()
    if (sizePx <= 0f || block.lines.isEmpty()) return
    val lineHeightPx = sizePx * lineRatio
    val style = TextStyle(
        color = fg,
        fontSize = (sizePx * scale).toSp(),
        lineHeight = (lineHeightPx * scale).toSp(),
        // 与服务器一致：字形在行盒内垂直居中（默认 Trim.Both 会把行盒裁到字形高度，
        // 使首行贴顶、整体上移约 (lineHeight-glyph)/2）
        lineHeightStyle = LineHeightStyle(
            alignment = LineHeightStyle.Alignment.Center,
            trim = LineHeightStyle.Trim.None,
        ),
        fontFamily = fontFamily ?: FontFamily.Default,
        // 服务器 PNG 渲染不处理斜体（raster._draw_text_item 只模拟粗体），
        // 这里保持正体以与"服务器图片"模式观感一致
        letterSpacing = (letterSpacingPx * scale).toSp(),
    )
    // 服务器约定"粗体不换字体文件"，渲染时用极小偏移二次描边模拟（raster._draw_text_item）。
    // 客户端对下载的单一字重 Typeface 请求 FontWeight.Bold 不会生效，这里同法模拟。
    val boldOffset = if (block.bold) max(1f, sizePx * scale / 32f) else 0f
    val x = originX + marginLeft * scale
    val maxLineWidth = availW * scale

    block.lines.forEachIndexed { index, line ->
        if (line.isEmpty()) return@forEachIndexed
        val y = originY + (marginTop + block.y + index * lineHeightPx) * scale
        val layout = textMeasurer.measure(
            text = AnnotatedString(line),
            style = style,
            maxLines = 1,
            softWrap = false,
        )
        val width = layout.size.width.toFloat()
        val factor = if (width > 0f) maxLineWidth / width else 1f
        val clipped = if (width > maxLineWidth && factor < 0.9f) {
            // 压缩超过 10% 仍放不下：按设计截断（不重排、不换行）
            Log.w(
                TAG,
                "文字直发行超宽（${width.roundToInt()}px > ${maxLineWidth.roundToInt()}px），已截断：" +
                    line.take(20),
            )
            textMeasurer.measure(
                text = AnnotatedString(line),
                style = style,
                maxLines = 1,
                softWrap = false,
                overflow = TextOverflow.Clip,
                constraints = Constraints(
                    maxWidth = maxLineWidth.roundToInt().coerceAtLeast(1)
                ),
            )
        } else {
            null
        }

        fun paint(shiftX: Float) {
            when {
                clipped != null -> drawText(clipped, topLeft = Offset(x + shiftX, y))

                width <= maxLineWidth || factor >= 1f ->
                    drawText(layout, topLeft = Offset(x + shiftX, y))

                else -> withTransform({
                    // 系统字体与服务器字体存在小幅字宽差：横向压缩（≤10%）保持整行可读
                    scale(scaleX = factor, scaleY = 1f, pivot = Offset(x + shiftX, y))
                }) {
                    drawText(layout, topLeft = Offset(x + shiftX, y))
                }
            }
        }

        paint(0f)
        if (boldOffset > 0f) paint(boldOffset)
    }
}

/**
 * 解析本页应使用的服务器字体（设计 3.2：客户端必须使用与服务器相同的字体文件）。
 *
 * - `family == "auto"` 时服务器用其默认字体排版，这里映射到字体列表里 `default=true` 的 id；
 * - 字体下载/加载失败返回 null：调用方回退系统字体并做压缩兜底。
 */
@Composable
private fun rememberServerFontFamily(
    family: String,
    fonts: List<FontInfoDto>,
): FontFamily? {
    val context = LocalContext.current
    val store = remember(context) { App.from(context).fontStore }
    val fontId = remember(family, fonts) {
        if (family.isBlank() || family.equals("auto", ignoreCase = true)) {
            fonts.firstOrNull { it.default }?.id.orEmpty()
        } else {
            family.trim()
        }
    }
    if (fontId.isEmpty()) return null
    val typeface by produceState<Typeface?>(
        initialValue = store.cached(fontId),
        key1 = fontId,
    ) {
        value = store.cached(fontId) ?: store.load(fontId)
    }
    return typeface?.let { FontFamily(it) }
}

/**
 * 按分量在 sRGB 空间混合（与服务器 `raster._blend` 同算法）。
 *
 * 不能用 `Color.lerp`：Compose 的 lerp 在 Oklab 空间插值，深色主题下色差肉眼可见。
 */
private fun blendSrgb(fg: Color, bg: Color, ratio: Float): Color {
    val t = ratio.coerceIn(0f, 1f)
    return Color(
        red = fg.red * (1f - t) + bg.red * t,
        green = fg.green * (1f - t) + bg.green * t,
        blue = fg.blue * (1f - t) + bg.blue * t,
        alpha = 1f,
    )
}

private const val TAG = "TextPage"
