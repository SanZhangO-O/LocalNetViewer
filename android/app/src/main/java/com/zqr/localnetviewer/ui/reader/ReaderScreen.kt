package com.zqr.localnetviewer.ui.reader

import android.app.Activity
import android.content.Context
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
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

    // 系统返回键（设计 6.3）：先关弹层 → 再退沉浸（出工具栏）→ 最后才离开阅读页。
    // 没有这个处理时，沉浸态下按返回会直接退出阅读页、连工具栏都看不到。
    BackHandler(enabled = true) {
        when {
            state.tocOpen -> vm.closeToc()
            state.settingsOpen -> vm.closeSettings()
            state.searchOpen -> vm.closeSearch()
            !state.overlayVisible -> vm.showOverlay()
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
    LaunchedEffect(state.currentPage) { zoomed = false }

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
                onError = vm::notify,
            )
        }

        if (state.preparing && state.canRender) {
            LinearProgressIndicator(
                modifier = Modifier
                    .fillMaxWidth()
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
            modifier = Modifier.align(Alignment.BottomCenter),
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
    // 跳得远时用 scrollToPage 直接落位：animateScrollToPage 会横扫全书
    // （第 1 章跳第 40 章要动画掠过几百页），固定版式下还会触发大量页面请求。
    LaunchedEffect(state.currentPage, state.totalPages) {
        val target = state.indexOfPage(state.currentPage).coerceIn(0, pageCount - 1)
        if (pagerState.currentPage != target) {
            val distance = kotlin.math.abs(target - pagerState.currentPage)
            if (distance > ANIMATED_JUMP_MAX_PAGES) {
                pagerState.scrollToPage(target)
            } else {
                pagerState.animateScrollToPage(target)
            }
        }
    }
    // 翻页停稳 → 取页元数据 + 预取 + 存进度（设计 6.4）
    LaunchedEffect(pagerState, state.layoutId) {
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
                listState.animateScrollToItem(target)
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
                AsyncImage(
                    model = pageUrl(index + 1),
                    contentDescription = null,
                    contentScale = ContentScale.FillWidth,
                    modifier = Modifier.fillMaxWidth(),
                )
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
