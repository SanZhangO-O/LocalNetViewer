package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import coil3.compose.LocalPlatformContext
import coil3.request.ImageRequest
import com.zqr.localnetviewer.data.net.dto.FontInfoDto
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis
import com.zqr.localnetviewer.ui.reader.ReaderTheme
import com.zqr.localnetviewer.ui.reader.TextPageState

private const val MAX_ZOOM = 4f
private const val ZOOM_EPSILON = 1.01f
/** 双击放大的目标倍率（设计 6.4）。 */
private const val DOUBLE_TAP_ZOOM = 2f

/**
 * 单页图片（设计 6.4 固定版式手势模型）。
 *
 * - 点击左 / 中 / 右三区：上一页 / 工具栏 / 下一页（由调用方决定方向）；
 * - 双击放大到 2x，放大后拖动平移（**放大后一定要能平移**，否则适应宽度模式下
 *   双击会把自己锁死：既不能平移、页内滚动容器又被换掉、Pager 也被禁用）；
 * - 平移范围有边界，图片不会被拖出屏幕；
 * - 适应宽度 → 纵向页内滚动；适应高度 → 横向页内滚动；
 * - 加载中有指示器、加载失败有"重试"占位（设计 6.4 的失败重试承诺）。
 */
@Composable
fun PageImage(
    url: String?,
    contentScale: ContentScale,
    innerScroll: InnerScrollAxis,
    zoomEnabled: Boolean,
    onZoomChanged: (Boolean) -> Unit,
    onTapLeft: () -> Unit,
    onTapRight: () -> Unit,
    onTapCenter: () -> Unit,
    onError: (String) -> Unit,
    modifier: Modifier = Modifier,
    /** 当前槽位对应的页身份：变化（翻页 / 跨页步长变化）时复位页内缩放 */
    pageKey: Any? = null,
    isCurrent: Boolean = true,
    /** 文字直发：非空时忽略 [url]，内容由客户端按 JSON 绘制（设计 3.2）。 */
    textPage: TextPageState? = null,
    textTheme: ReaderTheme = ReaderTheme.WHITE,
    textFonts: List<FontInfoDto> = emptyList(),
    /** 文字直发页加载失败时的重试（重新请求该页 JSON）。 */
    onTextRetry: () -> Unit = {},
) {
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    val zoomed = scale > ZOOM_EPSILON

    // 换页（槽位映射到另一页）时复位缩放，避免把上一页的放大状态带过来
    LaunchedEffect(pageKey) {
        scale = 1f
        offset = Offset.Zero
    }

    // 重试用：attempt 作为请求级参数改变请求身份（强制 Coil 重新发起网络请求），
    // 但显式固定 memory/disk cache key —— 不像旧方案那样在 URL 上拼 `_r=N`，
    // 那会为每次重试生成新缓存条目、且与预取的原始 URL 无法命中。
    // 失败结果不会被 Coil 缓存，所以同 URL 重新请求一定会再走网络。
    var attempt by remember(url) { mutableIntStateOf(0) }
    var failed by remember(url) { mutableStateOf(false) }
    var loading by remember(url) { mutableStateOf(false) }

    // 文字直发的加载/失败由 ViewModel 的页状态驱动（PNG 模式由 Coil 回调驱动）
    val isText = textPage != null
    val textFailure = textPage as? TextPageState.Failed
    val effectiveLoading = if (isText) textPage is TextPageState.Loading else loading
    val effectiveFailed = if (isText) textFailure != null else failed
    val failureMessage = if (isText) {
        textFailure?.message?.takeIf { it.isNotBlank() } ?: "页面加载失败"
    } else {
        "页面加载失败"
    }

    LaunchedEffect(zoomed) { onZoomChanged(zoomed) }

    // 离开当前页时复位缩放：Pager 会保留邻页，否则回到该页会"看着放大但可滑动"
    LaunchedEffect(isCurrent) {
        if (!isCurrent) {
            scale = 1f
            offset = Offset.Zero
        }
    }

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val widthPx = constraints.maxWidth.toFloat().coerceAtLeast(1f)
        val heightPx = constraints.maxHeight.toFloat().coerceAtLeast(1f)

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(zoomEnabled, widthPx, heightPx) {
                    detectTapGestures(
                        onTap = { position ->
                            when {
                                position.x < widthPx * 0.3f -> onTapLeft()
                                position.x > widthPx * 0.7f -> onTapRight()
                                else -> onTapCenter()
                            }
                        },
                        onDoubleTap = { position ->
                            if (zoomEnabled) {
                                if (scale > ZOOM_EPSILON) {
                                    scale = 1f
                                    offset = Offset.Zero
                                } else {
                                    // 以点击点为中心放大：保持该点在缩放前后位置不变
                                    // （pos = content * scale + offset，1x 时 content = pos）
                                    scale = DOUBLE_TAP_ZOOM
                                    offset = clampOffset(
                                        (position - Offset(widthPx / 2f, heightPx / 2f)) *
                                            (1f - DOUBLE_TAP_ZOOM),
                                        DOUBLE_TAP_ZOOM,
                                        widthPx,
                                        heightPx,
                                    )
                                }
                            }
                        },
                    )
                }
                .then(
                    // 双指捏合（1x 起即可放大）+ 放大后单指平移：自实现变换检测，
                    // **只在 ≥2 指、或已处于放大态时**消费事件（Initial pass），
                    // 1x 单指拖动不被消费 → Pager 翻页不受影响。
                    // 不能直接用 Modifier.transformable：它会把单指拖动当 pan 消费掉，
                    // Pager 永远收不到手势（"滑动翻不了页"的历史缺陷，见设计 6.4）。
                    if (zoomEnabled) {
                        Modifier.pointerInput(Unit) {
                            awaitEachGesture {
                                awaitFirstDown(requireUnconsumed = false)
                                var zooming = false
                                var panning = false
                                var prevCentroid = Offset.Zero
                                while (true) {
                                    val event = awaitPointerEvent(PointerEventPass.Initial)
                                    val pressedCount = event.changes.count { it.pressed }
                                    val allUp = event.changes.all { !it.pressed }
                                    val centroid = event.calculateCentroid()
                                    if (!zooming && pressedCount >= 2) {
                                        zooming = true
                                        panning = false
                                        prevCentroid = centroid
                                    } else if (!zooming && !panning && scale > ZOOM_EPSILON && pressedCount == 1) {
                                        panning = true
                                        prevCentroid = centroid
                                    }

                                    if (zooming && pressedCount >= 2) {
                                        val zoom = event.calculateZoom()
                                        val raw = scale * zoom
                                        val next =
                                            if (raw <= ZOOM_EPSILON) 1f else raw.coerceIn(1f, MAX_ZOOM)
                                        // 渲染模型：graphicsLayer 默认以图层中心 c 为 pivot，
                                        // screen(p) = (p − c)·s + c + offset。
                                        // 保持 prevCentroid（手势起点质心）下的内容点不动
                                        //（缩放前后 screen(x) 不变）：
                                        //   offset' = centroid − c − (prevCentroid − c − offset)·k
                                        // 其中 k = next / scale。k=1（纯双指平移）时退化为
                                        // offset' = offset + (centroid − prevCentroid)。
                                        // 缩回 1x 时直接复位——否则残留位移会让"翻页恢复
                                        // 但页面是歪的"（zoomed=false 已解锁 Pager）。
                                        val center = Offset(widthPx / 2f, heightPx / 2f)
                                        offset = if (next <= ZOOM_EPSILON) {
                                            Offset.Zero
                                        } else {
                                            val k = next / scale
                                            clampOffset(
                                                centroid - center -
                                                    (prevCentroid - center - offset) * k,
                                                next,
                                                widthPx,
                                                heightPx,
                                            )
                                        }
                                        scale = next
                                        event.changes.forEach { it.consume() }
                                    } else if (panning && scale > ZOOM_EPSILON && pressedCount == 1) {
                                        offset = clampOffset(
                                            offset + centroid - prevCentroid,
                                            scale,
                                            widthPx,
                                            heightPx,
                                        )
                                        event.changes.forEach { it.consume() }
                                    } else if (zooming || panning) {
                                        // 手指抬起离开模式：回不到放大态就复位偏移
                                        if (scale <= ZOOM_EPSILON) offset = Offset.Zero
                                        zooming = false
                                        panning = false
                                    }
                                    prevCentroid = centroid
                                    if (allUp) break
                                }
                            }
                        }
                    } else {
                        Modifier
                    }
                )
        ) {
            when {
                innerScroll == InnerScrollAxis.VERTICAL && !zoomed -> Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                ) {
                    PageContent(
                        model = url,
                        attempt = attempt,
                        contentScale = contentScale,
                        modifier = Modifier.fillMaxWidth(),
                        textPage = textPage,
                        textTheme = textTheme,
                        textFonts = textFonts,
                        onLoading = { loading = true },
                        onSuccess = { loading = false; failed = false },
                        onError = {
                            loading = false
                            failed = true
                            onError("页面加载失败")
                        },
                    )
                }

                innerScroll == InnerScrollAxis.HORIZONTAL && !zoomed -> Row(
                    modifier = Modifier
                        .fillMaxSize()
                        .horizontalScroll(rememberScrollState())
                ) {
                    PageContent(
                        model = url,
                        attempt = attempt,
                        contentScale = contentScale,
                        modifier = Modifier.fillMaxHeight(),
                        textPage = textPage,
                        textTheme = textTheme,
                        textFonts = textFonts,
                        onLoading = { loading = true },
                        onSuccess = { loading = false; failed = false },
                        onError = {
                            loading = false
                            failed = true
                            onError("页面加载失败")
                        },
                    )
                }

                else -> PageContent(
                    model = url,
                    attempt = attempt,
                    contentScale = contentScale,
                    modifier = Modifier
                        .fillMaxSize()
                        .graphicsLayer(
                            scaleX = scale,
                            scaleY = scale,
                            translationX = offset.x,
                            translationY = offset.y,
                        ),
                    textPage = textPage,
                    textTheme = textTheme,
                    textFonts = textFonts,
                    onLoading = { loading = true },
                    onSuccess = { loading = false; failed = false },
                    onError = {
                        loading = false
                        failed = true
                        onError("页面加载失败")
                    },
                )
            }

            // 占位：加载中给进度指示，避免"翻页白屏闪一下"看起来像卡住
            if (effectiveLoading && !effectiveFailed) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(24.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator(strokeWidth = 2.dp)
                }
            }

            // 失败占位 + 重试（设计 6.4：加载失败提供重试，而不是只弹一条 Snackbar）
            if (effectiveFailed) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(24.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = failureMessage,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        TextButton(onClick = {
                            if (isText) {
                                onTextRetry()
                            } else {
                                failed = false
                                attempt += 1
                            }
                        }) {
                            Text("重试")
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PageContent(
    model: String?,
    attempt: Int,
    contentScale: ContentScale,
    modifier: Modifier,
    textPage: TextPageState?,
    textTheme: ReaderTheme,
    textFonts: List<FontInfoDto>,
    onLoading: () -> Unit,
    onSuccess: () -> Unit,
    onError: () -> Unit,
) {
    // 文字直发：内容来自页面 JSON，加载/失败占位由外层状态驱动，这里不碰 Coil
    if (textPage != null) {
        if (textPage is TextPageState.Ready) {
            TextPageContent(
                page = textPage.page,
                theme = textTheme,
                fonts = textFonts,
                modifier = modifier,
            )
        }
        return
    }
    val context = LocalPlatformContext.current
    val request = remember(model, attempt) {
        when {
            model == null -> null
            // 重试：URL 加 "#attempt" 改变请求身份强制重新加载（fragment 不会发给
            // 服务器），缓存 key 显式固定为原始 URL —— 每次重试不会产生新缓存条目
            attempt > 0 -> ImageRequest.Builder(context)
                .data("$model#retry-$attempt")
                .memoryCacheKey(model)
                .diskCacheKey(model)
                .build()
            else -> ImageRequest.Builder(context)
                .data(model)
                .build()
        }
    }
    AsyncImage(
        model = request,
        contentDescription = null,
        contentScale = contentScale,
        modifier = modifier,
        onLoading = { onLoading() },
        onSuccess = { onSuccess() },
        onError = { onError() },
    )
}

/**
 * 把平移限制在"放大后超出视口的那部分"之内（Java/Android 常见的 overscroll 行为）。
 *
 * 不设边界时 `offset + panChange` 可以无限累加，图片会被拖出屏幕、只剩空白。
 */
private fun clampOffset(offset: Offset, scale: Float, widthPx: Float, heightPx: Float): Offset {
    val maxX = (widthPx * (scale - 1f)) / 2f
    val maxY = (heightPx * (scale - 1f)) / 2f
    return Offset(
        x = offset.x.coerceIn(-maxX, maxX),
        y = offset.y.coerceIn(-maxY, maxY),
    )
}
