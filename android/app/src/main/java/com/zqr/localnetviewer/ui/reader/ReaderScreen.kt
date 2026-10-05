package com.zqr.localnetviewer.ui.reader

import android.app.Activity
import android.content.Context
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import coil3.compose.AsyncImage
import coil3.imageLoader
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.ui.common.ErrorView
import com.zqr.localnetviewer.ui.common.LoadingView
import com.zqr.localnetviewer.ui.common.findActivity
import com.zqr.localnetviewer.ui.common.rememberStableInsets
import com.zqr.localnetviewer.ui.common.rememberWindowSizePx
import com.zqr.localnetviewer.ui.reader.components.PageImage
import com.zqr.localnetviewer.ui.reader.components.ReaderOverlay
import com.zqr.localnetviewer.ui.reader.components.SearchSheet
import com.zqr.localnetviewer.ui.reader.components.SettingsSheet
import com.zqr.localnetviewer.ui.reader.components.TocSheet
import kotlinx.coroutines.flow.distinctUntilChanged as flowDistinctUntilChanged
import kotlinx.coroutines.launch

/**
 * 阅读页（设计 6.3 / 6.4）。
 *
 * - 文本书：`HorizontalPager` 翻页，页码由 layout 章节表 + 当前页计算；
 * - 固定版式：适应模式 / 原图 / 阅读方向 / 双页跨页由设置面板控制（只改显示或 URL）；
 * - 条漫：纵向连续滚动切片；
 * - 沉浸模式：隐藏工具栏时隐藏系统栏并保持常亮。
 */
@Composable
fun ReaderScreen(
    bookId: String,
    onBack: () -> Unit,
    overrideViewModel: ReaderViewModel? = null,
) {
    val context = LocalContext.current
    val app = App.from(context)
    // 注意：参数名不能叫 viewModel，否则会遮蔽 androidx.lifecycle.viewmodel.compose.viewModel()
    val vm = overrideViewModel ?: viewModel<ReaderViewModel>(
        factory = viewModelFactory { initializer { ReaderViewModel(app, bookId) } }
    )
    val state by vm.state.collectAsStateWithLifecycle()

    // 系统返回键：先关弹层（目录/设置/搜索是模态，按返回先关掉）→ 直接离开阅读页。
    // 沉浸态不拦返回：浏览内容时按返回应直接退出，工具栏用点击屏幕中央呼出。
    BackHandler(enabled = true) {
        when {
            state.tocOpen -> vm.closeToc()
            state.settingsOpen -> vm.closeSettings()
            state.searchOpen -> vm.closeSearch()
            else -> onBack()
        }
    }

    // 视口与常态安全区上报（报告后由 ViewModel 决定是否需要重新布局）
    val viewport = rememberWindowSizePx()
    val insets = rememberStableInsets()
    val density = LocalDensity.current.density
    LaunchedEffect(viewport, insets, density) {
        vm.onViewport(viewport, insets, density)
    }

    // Kotlin 局部变量不允许前向引用：activity 必须先声明，DisposableEffect 才能引用它
    val activity = remember(context) { context.findActivity() }

    // onStop 立即落盘进度（设计 6.5）；离开阅读页时恢复系统栏，否则返回浏览页会"隐藏状态栏"
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, activity) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) vm.saveProgressNow()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            applyWindowState(activity, overlayVisible = true, keepScreenOn = false)
        }
    }

    LaunchedEffect(state.overlayVisible, state.settings.keepScreenOn) {
        applyWindowState(activity, state.overlayVisible, state.settings.keepScreenOn)
    }

    var zoomed by remember { mutableStateOf(false) }
    // 翻页时统一复位两处缩放状态：屏上 zoomed 与 ViewModel 的 renderScale。
    // 只复位 zoomed 会让后续页面继续以 scale=2.0 请求（renderScale 从不回落），
    // 既浪费流量又让预取/屏上缓存的解码尺寸翻倍。
    LaunchedEffect(state.currentPage) {
        zoomed = false
        vm.setZoomed(false)
    }

    val scope = rememberCoroutineScope()
    val snackbarHostState = remember { SnackbarHostState() }
    LaunchedEffect(state.message) {
        state.message?.let {
            snackbarHostState.showSnackbar(it)
            vm.consumeMessage()
        }
    }

    val theme = ReaderTheme.of(state.settings.theme)

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(theme.background))
    ) {
        when {
            state.error != null && !state.canRender -> ErrorView(
                message = state.error ?: "打开失败",
                onRetry = vm::retry,
                onSecondary = onBack,
                secondaryLabel = "返回",
            )

            !state.canRender -> LoadingView(
                text = if (state.preparing) "正在排版…" else "正在打开…"
            )

            state.strip -> StripReader(
                state = state,
                pageUrl = vm::pageUrl,
                onSettled = { vm.onPageSettled(it) },
                onTapCenter = vm::toggleOverlay,
                onError = vm::onPageImageFailed,
            )

            else -> PagedReader(
                state = state,
                pageUrl = vm::pageUrl,
                zoomed = zoomed,
                onZoomChanged = { value ->
                    zoomed = value
                    // 放大时需要以 scale=2.0 重新渲染当页，否则只是把 1x 位图放大（文字发虚）
                    vm.setZoomed(value)
                },
                onSettled = { vm.onPageSettled(it) },
                onPrev = vm::previousPage,
                onNext = vm::nextPage,
                onToggleOverlay = vm::toggleOverlay,
                onError = vm::onPageImageFailed,
            )
        }

        if (state.preparing && state.canRender) {
            LinearProgressIndicator(
                modifier = Modifier
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .align(Alignment.TopCenter)
            )
        }

        ReaderOverlay(
            state = state,
            onBack = onBack,
            onOpenToc = vm::openToc,
            onOpenSettings = vm::openSettings,
            onOpenSearch = vm::openSearch,
            onSeek = vm::jumpToPage,
        )

        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding(),
        )
    }

    if (state.tocOpen) {
        TocSheet(
            chapters = state.chapters,
            currentChapterIndex = state.chapterIndex,
            currentChapterTitle = state.chapterTitle,
            onSelect = vm::jumpToChapter,
            onDismiss = vm::closeToc,
        )
    }
    if (state.settingsOpen) {
        SettingsSheet(
            state = state,
            onSettingsChange = vm::updateSettings,
            onClearCache = { scope.launch { clearPageCache(context) } },
            onDismiss = vm::closeSettings,
        )
    }
    if (state.searchOpen) {
        SearchSheet(
            initialQuery = state.searchQuery,
            searching = state.searching,
            results = state.searchResults,
            limited = state.searchLimited,
            onSearch = vm::runSearch,
            onOpenResult = vm::openSearchResult,
            onDismiss = vm::closeSearch,
        )
    }
}

@Composable
private fun PagedReader(
    state: ReaderViewModel.UiState,
    pageUrl: (Int) -> String?,
    zoomed: Boolean,
    onZoomChanged: (Boolean) -> Unit,
    onSettled: (Int) -> Unit,
    onPrev: () -> Unit,
    onNext: () -> Unit,
    onToggleOverlay: () -> Unit,
    onError: (String) -> Unit,
) {
    val pageCount = state.spreadCount.coerceAtLeast(1)
    val pagerState = rememberPagerState(
        initialPage = state.indexOfPage(state.currentPage).coerceAtLeast(0),
        pageCount = { pageCount },
    )

    // 程序化跳页（目录 / 搜索 / 进度恢复）；用户滑动不受影响。
    // 墨水屏预设（默认）一律 scrollToPage 直接落位：动画跳页在墨水屏上会连续
    // 触发多次全屏刷新并留残影；在常规屏上长距离动画也会横扫全书触发大量页面请求。
    // 只有 lcd / custom（开动画）预设才对短距离跳页做动画。
    val animateJump = state.settings.animateTransitions
    LaunchedEffect(state.currentPage, state.totalPages, animateJump) {
        val target = state.indexOfPage(state.currentPage).coerceIn(0, pageCount - 1)
        if (pagerState.currentPage != target) {
            val distance = kotlin.math.abs(target - pagerState.currentPage)
            if (!animateJump || distance > ANIMATED_JUMP_MAX_PAGES) {
                pagerState.scrollToPage(target)
            } else {
                pagerState.animateScrollToPage(target)
            }
        }
    }
    // 翻页停稳 → 取页元数据 + 预取 + 存进度（设计 6.4）
    // key 必须含 spreadStep：pageOfIndex 依赖它（双页跨页），而切换 pairPages
    // 不改变 layoutId——漏掉会导致 effect 捕获旧步长，翻一"屏"被记成错误页码。
    LaunchedEffect(pagerState, state.layoutId, state.spreadStep) {
        snapshotFlow { pagerState.settledPage }
            .flowDistinctUntilChanged()
            .collect { onSettled(state.pageOfIndex(it)) }
    }

    HorizontalPager(
        state = pagerState,
        modifier = Modifier.fillMaxSize(),
        reverseLayout = state.reverseLayout,
        beyondViewportPageCount = 1,
        userScrollEnabled = !zoomed,
    ) { index ->
        val isCurrent = index == pagerState.settledPage
        PageImage(
            // 跨页模式下每一屏都以"首页"页码请求，服务器会一并渲染 page+1
            url = pageUrl(state.pageOfIndex(index)),
            // 页身份：映射到新页（翻页 / 跨页步长变化）时复位页内缩放
            pageKey = state.pageOfIndex(index),
            contentScale = contentScaleFor(state.settings, state.isFixed),
            innerScroll = innerScrollFor(state.settings, state.isFixed),
            zoomEnabled = true,
            onZoomChanged = { value ->
                // 只有当前页的缩放状态才影响 Pager 滑动
                if (isCurrent) onZoomChanged(value)
            },
            onTapLeft = { if (state.reverseLayout) onNext() else onPrev() },
            onTapRight = { if (state.reverseLayout) onPrev() else onNext() },
            onTapCenter = onToggleOverlay,
            onError = onError,
            isCurrent = isCurrent,
        )
    }
}

/** 超过这么多屏就不做动画跳转（长距离跳页动画既慢又会触发大量页面请求）。 */
private const val ANIMATED_JUMP_MAX_PAGES = 8

@Composable
private fun StripReader(
    state: ReaderViewModel.UiState,
    pageUrl: (Int) -> String?,
    onSettled: (Int) -> Unit,
    onTapCenter: () -> Unit,
    onError: (String) -> Unit,
) {
    val pageCount = state.totalPages.coerceAtLeast(1)
    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = (state.currentPage - 1).coerceAtLeast(0)
    )
    // 程序化跳转期间要抑制"首个可见项"回写 currentPage，
    // 否则动画途中 currentPage 被改写 → LaunchedEffect 重启 → 动画被取消（跳转只能走几屏）
    var pendingJump by remember { mutableStateOf<Int?>(null) }

    LaunchedEffect(state.currentPage, state.totalPages) {
        val target = (state.currentPage - 1).coerceIn(0, pageCount - 1)
        if (listState.firstVisibleItemIndex != target) {
            pendingJump = target
            try {
                // 墨水屏预设（默认）直接落位；开动画的预设才平滑滚动跳转
                if (state.settings.animateTransitions) {
                    listState.animateScrollToItem(target)
                } else {
                    listState.scrollToItem(target)
                }
            } finally {
                pendingJump = null
            }
        }
    }
    LaunchedEffect(listState, state.layoutId) {
        snapshotFlow { listState.firstVisibleItemIndex }
            .flowDistinctUntilChanged()
            .collect { index ->
                if (pendingJump == null) onSettled(index + 1)
            }
    }

    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxSize(),
    ) {
        items(count = pageCount, key = { it }) { index ->
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .pointerInput(index) {
                        detectTapGestures { onTapCenter() }
                    }
            ) {
                StripPageImage(url = pageUrl(index + 1), onError = onError)
            }
        }
    }
}

/**
 * 条漫单页：与图文模式同等对待——加载中有指示器、失败有重试占位
 * （不能裸用 AsyncImage：切片加载失败无提示，看起来像"这本书缺页"）。
 * 条漫切片不需要缩放手势（整条连续滚动模型），点击留给"呼出工具栏"。
 */
@Composable
private fun StripPageImage(url: String?, onError: (String) -> Unit) {
    var attempt by remember(url) { mutableIntStateOf(0) }
    var failed by remember(url) { mutableStateOf(false) }
    var loading by remember(url) { mutableStateOf(false) }
    Box(modifier = Modifier.fillMaxWidth()) {
        // key(attempt)：重试时强制重建请求（失败不会被 Coil 缓存，重发即可）
        key(attempt) {
            AsyncImage(
                model = url,
                contentDescription = null,
                contentScale = ContentScale.FillWidth,
                modifier = Modifier.fillMaxWidth(),
                onLoading = { loading = true },
                onSuccess = { loading = false; failed = false },
                onError = {
                    loading = false
                    failed = true
                    // 切片 404 也可能是 layout 已失效：上报给 ViewModel，走页元数据触发的自动重建
                    onError("切片加载失败")
                },
            )
        }
        if (loading && !failed) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(180.dp),
                contentAlignment = Alignment.Center,
            ) {
                CircularProgressIndicator(strokeWidth = 2.dp)
            }
        }
        if (failed) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text("切片加载失败", style = MaterialTheme.typography.bodyMedium)
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

/** 沉浸模式 + 屏幕常亮（设计 6.3）。 */
private fun applyWindowState(
    activity: Activity?,
    overlayVisible: Boolean,
    keepScreenOn: Boolean,
) {
    val window = activity?.window ?: return
    if (keepScreenOn) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    } else {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
    val controller = WindowCompat.getInsetsController(window, window.decorView)
    controller.systemBarsBehavior =
        WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    if (overlayVisible) {
        controller.show(WindowInsetsCompat.Type.systemBars())
    } else {
        controller.hide(WindowInsetsCompat.Type.systemBars())
    }
}

/** 清理 Coil 页面缓存（设计 6.4：设置页提供"清理页面缓存"入口）。 */
private suspend fun clearPageCache(context: Context) {
    val loader = context.imageLoader
    runCatching { loader.memoryCache?.clear() }
    runCatching { loader.diskCache?.clear() }
}
