package com.zqr.localnetviewer.ui.reader

import androidx.compose.ui.geometry.Size
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.ScaleFactor
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
import kotlin.math.min
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
 * SCREEN 是默认：宽高比与视口接近时等比放大铺满（裁边 ≤[FILL_SCREEN_MAX_CROP]），
 * 比例差距大时退化为完整显示（保持原比例，不裁掉大半画面，见 [FillScreenScale]）；
 * PAGE 始终完整显示整页；WIDTH / HEIGHT 单向等比撑满 + 页内滚动。
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

/**
 * "铺满屏幕"的显示策略（保持原比例）。
 *
 * 图片与视口宽高比接近（等比覆盖的裁边不超过 [FILL_SCREEN_MAX_CROP]）时放大覆盖
 * 整块屏幕（`Crop` 语义）；比例差距大时退化为完整显示（`Fit` 语义）——直接
 * `ContentScale.Crop` 会在视口与图片比例差距大时裁掉大半画面（例如竖屏手机看
 * 横向跨页、横屏看单页都只能看到画面中心的一小部分，真实反馈）。
 */
val FillScreenScale: ContentScale = object : ContentScale {
    override fun computeScaleFactor(srcSize: Size, dstSize: Size): ScaleFactor {
        if (srcSize.width <= 0f || srcSize.height <= 0f ||
            dstSize.width <= 0f || dstSize.height <= 0f
        ) {
            // 尺寸未知（加载中）：先 1:1，测量完成后会再次计算
            return ScaleFactor(1f, 1f)
        }
        val cover = max(dstSize.width / srcSize.width, dstSize.height / srcSize.height)
        val fit = min(dstSize.width / srcSize.width, dstSize.height / srcSize.height)
        val visibleWidth = min(srcSize.width, dstSize.width / cover)
        val visibleHeight = min(srcSize.height, dstSize.height / cover)
        val cropX = 1f - visibleWidth / srcSize.width
        val cropY = 1f - visibleHeight / srcSize.height
        return if (cropX <= FILL_SCREEN_MAX_CROP && cropY <= FILL_SCREEN_MAX_CROP) {
            ScaleFactor(cover, cover)
        } else {
            ScaleFactor(fit, fit)
        }
    }
}

/** 覆盖裁边上限（任一方向）；超过即退化为完整显示，防止裁掉大半画面。 */
private const val FILL_SCREEN_MAX_CROP = 0.08f

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
        FitMode.SCREEN -> FillScreenScale
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
