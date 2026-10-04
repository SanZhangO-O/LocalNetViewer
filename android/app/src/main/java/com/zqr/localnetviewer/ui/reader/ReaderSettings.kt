package com.zqr.localnetviewer.ui.reader

import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.IntSize
import com.zqr.localnetviewer.data.net.dto.FontSpecDto
import com.zqr.localnetviewer.data.net.dto.ImageSpecDto
import com.zqr.localnetviewer.data.net.dto.LayoutProfileDto
import com.zqr.localnetviewer.data.net.dto.MarginDto
import com.zqr.localnetviewer.data.net.dto.ParagraphSpecDto
import com.zqr.localnetviewer.data.net.dto.ViewportDto
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.common.InsetsPx
import kotlin.math.max
import kotlin.math.roundToInt

/** 主题配色与服务器端预设一一对应（设计 4.7）。 */
enum class ReaderTheme(val id: String, val background: Long, val foreground: Long) {
    WHITE("white", 0xFFFFFFFF, 0xFF222222),
    PAPER("paper", 0xFFF5F0E6, 0xFF2B2B2B),
    SEPIA("sepia", 0xFFF0E2C8, 0xFF3B2E1E),
    GREEN("green", 0xFFCCE8CF, 0xFF1F2A1F),
    DARK("dark", 0xFF111418, 0xFFC8C8C8);

    companion object {
        fun of(id: String): ReaderTheme = entries.firstOrNull { it.id == id } ?: WHITE
    }
}

/**
 * 固定版式适应模式（只改客户端显示策略，不触发重新分页，设计 6.4）。
 *
 * SCREEN 是默认：整页拉伸铺满整块屏幕（不留黑边，漫画优先占屏）；
 * PAGE 完整显示整页但按比例留边；WIDTH / HEIGHT 单向撑满 + 页内滚动。
 */
enum class FitMode(val id: String, val label: String) {
    SCREEN("screen", "铺满屏幕"),
    PAGE("page", "整页"),
    WIDTH("width", "适应宽度"),
    HEIGHT("height", "适应高度");

    companion object {
        fun of(id: String): FitMode = entries.firstOrNull { it.id == id } ?: SCREEN
    }
}

enum class ReadingDirection(val id: String, val label: String) {
    LTR("ltr", "左→右"),
    RTL("rtl", "右→左（日漫）");

    companion object {
        fun of(id: String): ReadingDirection = entries.firstOrNull { it.id == id } ?: LTR
    }
}

/**
 * 工具栏预留高度（设计 4.7 / 6.4）。
 *
 * 顶栏与底栏都**始终预留**：工具栏是半透明叠加层，如果不多留出这块高度，
 * 打开一本书时（工具栏默认可见）第一行正文会被顶栏盖住 —— 底栏早就预留了 48dp，
 * 顶栏漏了。始终预留的代价是沉浸态下上下各留一条主题背景，换来的是
 * "显隐工具栏不触发重新分页"（设计 4.6 的明确意图）。
 */
const val OVERLAY_BOTTOM_DP = 48
const val OVERLAY_TOP_DP = 64

/**
 * 由客户端设置 + 屏幕几何构造排版参数（设计 4.6）。
 *
 * 全部换算为**物理像素**：sp/dp × density；边距取"用户边距"与"常态系统栏 inset"的较大值，
 * 保证任何系统栏状态下正文都不被遮挡。
 */
fun buildLayoutProfile(
    viewport: IntSize,
    settings: AppSettings,
    density: Float,
    insets: InsetsPx,
    bookMode: String,
): LayoutProfileDto {
    val fontPx = (settings.fontSizeSp * density).roundToInt().coerceIn(10, 400)
    val marginH = (settings.marginHorizontalDp * density).roundToInt().coerceAtLeast(0)
    val marginV = (settings.marginVerticalDp * density).roundToInt().coerceAtLeast(0)
    val overlayBottom = (OVERLAY_BOTTOM_DP * density).roundToInt()
    val overlayTop = (OVERLAY_TOP_DP * density).roundToInt()
    val effectiveMode = effectiveMode(settings, bookMode)
    return LayoutProfileDto(
        viewport = ViewportDto(viewport.width, viewport.height),
        margin = MarginDto(
            left = max(marginH, insets.left),
            right = max(marginH, insets.right),
            top = max(marginV, insets.top) + overlayTop,
            bottom = max(marginV, insets.bottom) + overlayBottom,
        ),
        font = FontSpecDto(
            family = settings.fontFamily,
            sizePx = fontPx,
            lineHeight = settings.lineHeight.coerceIn(1.0f, 4.0f),
            letterSpacingPx = 0f,
        ),
        paragraph = ParagraphSpecDto(
            spacingPx = settings.paragraphSpacingSp * density,
            firstLineIndentChars = 2,
        ),
        image = ImageSpecDto(maxWidthRatio = 1f, maxHeightRatio = 1f),
        chapterNewPage = true,
        mode = effectiveMode,
        stripOverlapPx = 0,
    )
}

/** 文本书永远是 flow；漫画/PDF 可选条漫切片（设计 3.6）。 */
fun effectiveMode(settings: AppSettings, bookMode: String): String {
    if (bookMode == "flow") return "flow"
    return if (settings.stripMode) "strip" else "fixed"
}

/**
 * 影响分页结果的输入签名：变化才重新 `/layout`（theme 与 render_scale 不参与，设计 4.6）。
 */
fun profileSignature(
    viewport: IntSize,
    settings: AppSettings,
    density: Float,
    insets: InsetsPx,
    bookMode: String,
): String = listOf(
    viewport.width,
    viewport.height,
    density,
    insets.left,
    insets.top,
    insets.right,
    insets.bottom,
    settings.geometrySignature(),
    effectiveMode(settings, bookMode),
).joinToString("|")

/** 页面显示策略：固定版式按适应模式，文本书整页 Fit。 */
fun contentScaleFor(settings: AppSettings, isFixed: Boolean): ContentScale {
    if (!isFixed) return ContentScale.Fit
    return when (FitMode.of(settings.fitMode)) {
        FitMode.SCREEN -> ContentScale.FillBounds
        FitMode.PAGE -> ContentScale.Fit
        FitMode.WIDTH -> ContentScale.FillWidth
        FitMode.HEIGHT -> ContentScale.FillHeight
    }
}

/**
 * 页内滚动轴（设计 6.4）。
 *
 * 必须区分轴向：适应宽度是"宽度撑满视口、纵向滚动"，适应高度是"高度撑满视口、
 * 横向滚动"。早期实现把两者都做成 `fillMaxWidth()` + `verticalScroll()`，于是
 * `ContentScale.FillHeight` 拿不到视口高度约束，适应高度实际等价于适应宽度。
 */
enum class InnerScrollAxis { NONE, VERTICAL, HORIZONTAL }

fun innerScrollFor(settings: AppSettings, isFixed: Boolean): InnerScrollAxis {
    if (!isFixed) return InnerScrollAxis.NONE
    return when (FitMode.of(settings.fitMode)) {
        FitMode.SCREEN -> InnerScrollAxis.NONE
        FitMode.PAGE -> InnerScrollAxis.NONE
        FitMode.WIDTH -> InnerScrollAxis.VERTICAL
        FitMode.HEIGHT -> InnerScrollAxis.HORIZONTAL
    }
}

/** 兼容旧调用点：是否需要页内滚动。 */
fun needsInnerScroll(settings: AppSettings, isFixed: Boolean): Boolean =
    innerScrollFor(settings, isFixed) != InnerScrollAxis.NONE
