package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
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
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import com.zqr.localnetviewer.ui.reader.InnerScrollAxis

private const val MAX_ZOOM = 4f
private const val ZOOM_EPSILON = 1.01f

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
    isCurrent: Boolean = true,
) {
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    val zoomed = scale > ZOOM_EPSILON

    // 重试用：URL 追加一次性查询参数（服务器忽略未知参数），强制 Coil 重新请求
    var attempt by remember(url) { mutableIntStateOf(0) }
    var failed by remember(url) { mutableStateOf(false) }
    var loading by remember(url) { mutableStateOf(false) }
    val model = remember(url, attempt) {
        when {
            url == null -> null
            attempt == 0 -> url
            url.contains('?') -> "$url&_r=$attempt"
            else -> "$url?_r=$attempt"
        }
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

        val transformState = rememberTransformableState { zoomChange, panChange, _ ->
            // 小于阈值的缩放直接归一到 1f：否则 scale 落在 (1.0, 1.01] 时 zoomed 仍为
            // false，transformable 会在手势中途被摘掉、偏移被丢弃，而且没有任何分支
            // 应用 graphicsLayer —— 表现为"捏合了但画面没反应"
            val raw = scale * zoomChange
            val next = if (raw <= ZOOM_EPSILON) 1f else raw.coerceIn(1f, MAX_ZOOM)
            scale = next
            offset = if (next > ZOOM_EPSILON) {
                clampOffset(offset + panChange, next, widthPx, heightPx)
            } else {
                Offset.Zero
            }
        }

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
                        onDoubleTap = {
                            if (zoomEnabled) {
                                if (scale > ZOOM_EPSILON) {
                                    scale = 1f
                                    offset = Offset.Zero
                                } else {
                                    scale = 2f
                                }
                            }
                        },
                    )
                }
                .then(
                    // 只在**放大之后**才挂 transformable。
                    //
                    // 设计 6.4：整页模式「Pager 直接翻页」、适应宽度/高度模式「拖动主轴为
                    // 水平时交给 Pager 翻页」。而 `Modifier.transformable` 一旦挂上就会消费
                    // 单指拖动（它把横向拖动当成 pan），Pager 永远收不到手势 ——
                    // 结果是"左右滑动翻不了页"。点击左右分区仍然有效，所以这个问题
                    // 在只做静态检查时几乎不可能发现。
                    //
                    // 1x 时没有任何东西需要平移，缩放靠双击（设计 6.4 只规定了双击放大）；
                    // 放大后由本修饰符负责平移，同时 Pager 被 userScrollEnabled=false 锁住。
                    if (zoomEnabled && zoomed) {
                        Modifier.transformable(state = transformState)
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
                        model = model,
                        contentScale = contentScale,
                        modifier = Modifier.fillMaxWidth(),
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
                        model = model,
                        contentScale = contentScale,
                        modifier = Modifier.fillMaxHeight(),
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
                    model = model,
                    contentScale = contentScale,
                    modifier = Modifier
                        .fillMaxSize()
                        .graphicsLayer(
                            scaleX = scale,
                            scaleY = scale,
                            translationX = offset.x,
                            translationY = offset.y,
                        ),
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
            if (loading && !failed) {
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
            if (failed) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(24.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = "页面加载失败",
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        TextButton(onClick = {
                            failed = false
                            attempt += 1
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
    model: Any?,
    contentScale: ContentScale,
    modifier: Modifier,
    onLoading: () -> Unit,
    onSuccess: () -> Unit,
    onError: () -> Unit,
) {
    AsyncImage(
        model = model,
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
