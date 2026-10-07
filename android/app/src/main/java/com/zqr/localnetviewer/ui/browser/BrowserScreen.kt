package com.zqr.localnetviewer.ui.browser

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowLeft
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.automirrored.filled.LastPage
import androidx.compose.material.icons.filled.Collections
import androidx.compose.material.icons.filled.FirstPage
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.PlayCircle
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import kotlinx.coroutines.flow.distinctUntilChanged
import coil3.compose.AsyncImage
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.dto.FileEntryDto
import com.zqr.localnetviewer.ui.common.EmptyView
import com.zqr.localnetviewer.ui.common.ErrorView
import com.zqr.localnetviewer.ui.common.LoadingView
import com.zqr.localnetviewer.ui.common.formatSize

/** 浏览页（设计 6.3）：面包屑 + 目录/书籍列表 + 底部分页栏 + "打开为图集"入口。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BrowserScreen(
    onOpenBook: (String) -> Unit,
    onOpenArchive: (String) -> Unit = {},
    onOpenMedia: (url: String, title: String, audioOnly: Boolean) -> Unit = { _, _, _ -> },
    onDisconnect: () -> Unit,
    overrideViewModel: BrowserViewModel? = null,
) {
    val context = LocalContext.current
    val app = App.from(context)
    // 注意：参数名不能叫 viewModel，否则会遮蔽 androidx.lifecycle.viewmodel.compose.viewModel()
    val vm = overrideViewModel ?: viewModel<BrowserViewModel>(
        factory = viewModelFactory { initializer { BrowserViewModel(app) } }
    )
    val state by vm.state.collectAsStateWithLifecycle()
    val openBook by vm.openBook.collectAsStateWithLifecycle()
    val openMedia by vm.openMedia.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }

    // 系统返回键 = 返回上一页（恢复离开时的页码与滚动位置）；无历史时回到上一级目录，
    // 已在书库根目录且无历史时交给系统（退出 App）
    BackHandler(enabled = state.parent != null || state.backStackDepth > 0) { vm.navigateBack() }

    LaunchedEffect(openBook) {
        openBook?.let {
            vm.consumeOpenBook()
            onOpenBook(it)
        }
    }
    LaunchedEffect(openMedia) {
        openMedia?.let {
            vm.consumeOpenMedia()
            onOpenMedia(it.url, it.title, it.audioOnly)
        }
    }
    LaunchedEffect(state.message) {
        state.message?.let {
            snackbarHostState.showSnackbar(it)
            vm.consumeMessage()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            text = state.currentName,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        // 服务器名/版本/书库数（设计 6.3）：连接页在连接成功后立刻被
                        // 移出返回栈，信息放在这里才真的看得见
                        val subtitle = when {
                            state.scanning && state.serverSummary.isNotEmpty() ->
                                "书库扫描中… · ${state.serverSummary}"
                            state.scanning -> "书库扫描中…"
                            else -> state.serverSummary
                        }
                        if (subtitle.isNotEmpty()) {
                            Text(
                                text = subtitle,
                                style = MaterialTheme.typography.labelSmall,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                },
                navigationIcon = {
                    // 有目录历史就显示（即使在根目录，也可能还有"返回"可走，如面包屑跳回根后）
                    if (state.parent != null || state.backStackDepth > 0) {
                        IconButton(onClick = vm::navigateBack) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回上一页")
                        }
                    }
                },
                actions = {
                    IconButton(onClick = vm::refresh) {
                        Icon(Icons.Default.Refresh, contentDescription = "刷新")
                    }
                    TextButton(onClick = onDisconnect) { Text("断开") }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                // 底部分页栏含输入框：edge-to-edge 下键盘不会自动缩窗（API 30+），
                // 必须 imePadding 才能在输入时露出分页栏（旧系统走窗口 resize，此项为 0）
                .imePadding()
        ) {
            Breadcrumb(
                path = state.path,
                onNavigate = { vm.load(it) },
            )
            HorizontalDivider()
            if (state.scanning || (state.loading && state.entries.isNotEmpty())) {
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }

            when {
                state.loading && state.entries.isEmpty() -> LoadingView()
                state.error != null -> ErrorView(
                    message = state.error ?: "加载失败",
                    onRetry = vm::refresh,
                    onSecondary = onDisconnect,
                    secondaryLabel = "断开连接",
                )
                state.entries.isEmpty() -> EmptyView("此目录为空")
                else -> {
                    // rememberLazyListState 内部用 rememberSaveable：进阅读页再返回时
                    // 位置由 Saveable 恢复，因此"数据版本没变"时不能强行回顶
                    val listState = rememberLazyListState()
                    // 持续把滚动位置上报给 ViewModel（非 Compose 状态，避免每帧重组），
                    // 离开目录时用它快照，"返回上一页"时才能恢复原位
                    LaunchedEffect(listState) {
                        snapshotFlow {
                            listState.firstVisibleItemIndex to listState.firstVisibleItemScrollOffset
                        }.collect { (index, offset) -> vm.updateScroll(index, offset) }
                    }
                    // 数据版本变化 = 新一页/新目录数据真正到达：有待恢复位置就恢复
                    // （返回上一页），否则回到顶部。版本已处理过（如从阅读页返回重建
                    // 组合，effect 重新执行）时不滚动，保留 Saveable 恢复的位置。
                    LaunchedEffect(state.dataVersion) {
                        val restore = vm.pendingScrollAction(state.dataVersion)
                        if (restore != null) {
                            // 内容可能在离开后变化：越界位置收敛到末项，避免 scrollToItem 异常
                            val maxIndex = (
                                state.entries.size +
                                    if (state.canOpenCurrentAsGallery) 1 else 0
                                ) - 1
                            val index = restore.index.coerceIn(0, maxIndex.coerceAtLeast(0))
                            val offset = if (index == restore.index) restore.offset else 0
                            listState.scrollToItem(index, offset.coerceAtLeast(0))
                        } else {
                            listState.scrollToItem(0)
                        }
                    }
                    Column(modifier = Modifier.fillMaxSize()) {
                        LazyColumn(
                            modifier = Modifier.weight(1f),
                            state = listState,
                        ) {
                            if (state.canOpenCurrentAsGallery) {
                                item(key = "current-gallery") {
                                    GalleryEntryCard(
                                        title = "查看本文件夹图片（${state.gallery?.imageCount ?: 0} 张）",
                                        subtitle = "不打开文件，直接把当前文件夹作为图集阅读",
                                        enabled = !state.opening,
                                        onClick = { vm.openFolderAsGallery(state.path) },
                                    )
                                }
                            }
                            // key 带 index 前缀：服务器在分页边界/重命名竞态下可能返回重复
                            // path，LazyColumn 的重复 key 会直接抛 IllegalArgumentException
                            // （与 SearchSheet 同样的防护）
                            itemsIndexed(state.entries, key = { i, entry -> "${entry.path}#$i" }) { _, entry ->
                                EntryRow(
                                    entry = entry,
                                    coverUrl = if (entry.isBook) vm.coverUrl(entry, 240) else null,
                                    onClick = {
                                        // 压缩包打不开（无图片页 / 缺后端）时，"查看内容"升级为主操作
                                        val archiveId = entry.bookId
                                        if (entry.isArchiveBook() && !entry.openable && archiveId != null) {
                                            onOpenArchive(archiveId)
                                        } else {
                                            vm.onEntryClick(entry)
                                        }
                                    },
                                    onOpenAsGallery = { vm.openFolderAsGallery(entry.path) },
                                    onOpenArchive = onOpenArchive,
                                    opening = state.opening,
                                )
                                HorizontalDivider()
                            }
                        }
                        if (state.totalPages > 1) {
                            HorizontalDivider()
                            PaginationBar(
                                page = state.page,
                                totalPages = state.totalPages,
                                total = state.total,
                                enabled = !state.loading,
                                onFirst = vm::firstPage,
                                onPrev = vm::prevPage,
                                onNext = vm::nextPage,
                                onLast = vm::lastPage,
                                onJump = vm::goToPage,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Breadcrumb(path: String, onNavigate: (String) -> Unit) {
    val segments = path.trim('/').split('/').filter { it.isNotEmpty() }
    // 深层路径横向滚动（自动滚到末段），而不是把每段压到不可见。
    // maxValue 要等布局完成后才非零：用 snapshotFlow 监听它变化后再滚动，
    // 直接在 LaunchedEffect 里读会拿到上一帧的值（首次/加深时滚不到末尾）。
    val scrollState = rememberScrollState()
    LaunchedEffect(segments) {
        snapshotFlow { scrollState.maxValue }
            .distinctUntilChanged()
            .collect { max -> if (max > 0) scrollState.scrollTo(max) }
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(scrollState)
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = "书库",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier
                .heightIn(min = 36.dp)
                .wrapContentHeight(Alignment.CenterVertically)
                .clip(RoundedCornerShape(4.dp))
                .clickable { onNavigate("/") }
                .padding(horizontal = 8.dp, vertical = 6.dp),
        )
        var accumulated = ""
        segments.forEach { segment ->
            accumulated = "$accumulated/$segment"
            val target = accumulated
            Text(
                text = " / ",
                style = MaterialTheme.typography.labelLarge,
            )
            Text(
                text = segment,
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .heightIn(min = 36.dp)
                    .wrapContentHeight(Alignment.CenterVertically)
                    .clip(RoundedCornerShape(4.dp))
                    .clickable { onNavigate(target) }
                    .padding(horizontal = 8.dp, vertical = 6.dp),
            )
        }
    }
}

@Composable
private fun GalleryEntryCard(
    title: String,
    subtitle: String,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp)
            .clickable(enabled = enabled, onClick = onClick)
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.Collections, contentDescription = null)
            Spacer(Modifier.width(12.dp))
            Column {
                Text(text = title, style = MaterialTheme.typography.titleSmall)
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun EntryRow(
    entry: FileEntryDto,
    coverUrl: String?,
    onClick: () -> Unit,
    onOpenAsGallery: () -> Unit,
    onOpenArchive: (String) -> Unit,
    opening: Boolean,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        EntryLeading(entry = entry, coverUrl = coverUrl)

        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = entry.name,
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            val description = buildString {
                if (entry.isBook) {
                    append(entry.format?.uppercase() ?: "未知格式")
                    val size = formatSize(entry.size)
                    if (size.isNotEmpty()) append(" · ").append(size)
                } else if (entry.isDir) {
                    val count = entry.gallery?.imageCount ?: 0
                    append(if (count > 0) "文件夹 · $count 张图片" else "文件夹")
                } else if (entry.isImageFile) {
                    append("图片")
                } else if (entry.isMedia) {
                    append(if (entry.isAudio) "音频" else "视频")
                    val size = formatSize(entry.size)
                    if (size.isNotEmpty()) append(" · ").append(size)
                }
                if (!entry.openable) {
                    append(" · ").append(entry.reason ?: "不可打开")
                }
            }
            if (description.isNotEmpty()) {
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (entry.openable) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        MaterialTheme.colorScheme.error
                    },
                )
            }
        }

        if ((entry.isDir || entry.isImageFile) && entry.gallery?.canOpenAsBook == true) {
            TextButton(onClick = onOpenAsGallery, enabled = !opening) {
                Text("打开为图集")
            }
        }

        // 压缩包书籍：默认点按仍作为书打开，"内容"进压缩包浏览（设计 7.6）。
        // 旧服务器没有 contents API：请求 404 时界面显示 Snackbar 错误，不崩溃。
        val archiveId = entry.bookId
        if (entry.isArchiveBook() && archiveId != null) {
            TextButton(
                onClick = { onOpenArchive(archiveId) },
                enabled = !opening,
            ) {
                Text(if (entry.openable) "内容" else "查看内容")
            }
        }
    }
}

@Composable
private fun EntryLeading(entry: FileEntryDto, coverUrl: String?) {
    val modifier = Modifier
        .size(width = 44.dp, height = 58.dp)
        .clip(RoundedCornerShape(4.dp))
    when {
        entry.isBook && coverUrl != null -> {
            AsyncImage(
                model = coverUrl,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = modifier,
            )
        }
        entry.isDir -> IconBox(Icons.Default.Folder)
        entry.isImageFile -> IconBox(Icons.Default.Image)
        entry.isMedia -> IconBox(Icons.Default.PlayCircle)
        else -> IconBox(Icons.AutoMirrored.Filled.InsertDriveFile)
    }
}

@Composable
private fun IconBox(icon: ImageVector) {
    Box(
        modifier = Modifier
            .size(width = 44.dp, height = 58.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * 底部分页栏（设计 6.3）：第一页 / 上一页 / 下一页 / 最后一页 + 页码指示 + 页码跳转。
 * 越界页码由 ViewModel 收敛；加载中整栏禁用，避免连点发出竞态请求。
 */
@Composable
private fun PaginationBar(
    page: Int,
    totalPages: Int,
    total: Int,
    enabled: Boolean,
    onFirst: () -> Unit,
    onPrev: () -> Unit,
    onNext: () -> Unit,
    onLast: () -> Unit,
    onJump: (Int) -> Unit,
) {
    var jumpText by remember { mutableStateOf("") }
    val focusManager = LocalFocusManager.current

    fun submitJump() {
        val target = jumpText.trim().toIntOrNull() ?: return
        onJump(target)
        jumpText = ""
        focusManager.clearFocus()
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 4.dp, vertical = 2.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onFirst, enabled = enabled && page > 1) {
                Icon(Icons.Filled.FirstPage, contentDescription = "第一页")
            }
            IconButton(onClick = onPrev, enabled = enabled && page > 1) {
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowLeft, contentDescription = "上一页")
            }
            Text(
                text = "第 $page / $totalPages 页 · 共 $total 项",
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.weight(1f),
            )
            IconButton(onClick = onNext, enabled = enabled && page < totalPages) {
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = "下一页")
            }
            IconButton(onClick = onLast, enabled = enabled && page < totalPages) {
                Icon(Icons.AutoMirrored.Filled.LastPage, contentDescription = "最后一页")
            }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
        ) {
            OutlinedTextField(
                value = jumpText,
                onValueChange = { jumpText = it.filter(Char::isDigit).take(7) },
                modifier = Modifier.width(140.dp),
                label = { Text("页码") },
                // 聚焦后显示有效范围；label 只写"页码"，避免大 N 时把 140dp 的标签撑爆
                placeholder = { Text("1 - $totalPages") },
                singleLine = true,
                enabled = enabled,
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Number,
                    imeAction = ImeAction.Done,
                ),
                keyboardActions = KeyboardActions(onDone = { submitJump() }),
            )
            Spacer(Modifier.width(8.dp))
            TextButton(onClick = { submitJump() }, enabled = enabled) {
                Text("跳转")
            }
        }
    }
}
