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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.filled.Collections
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
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

/** 浏览页（设计 6.3）：面包屑 + 目录/书籍列表 + "打开为图集"入口。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BrowserScreen(
    onOpenBook: (String) -> Unit,
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
    val snackbarHostState = remember { SnackbarHostState() }

    // 系统返回键 = 回到上一级目录；已在书库根目录时交给系统（退出 App）
    BackHandler(enabled = state.parent != null) { vm.navigateUp() }

    LaunchedEffect(openBook) {
        openBook?.let {
            vm.consumeOpenBook()
            onOpenBook(it)
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
                            state.scanning && app.serverSummary.isNotEmpty() ->
                                "书库扫描中… · ${app.serverSummary}"
                            state.scanning -> "书库扫描中…"
                            else -> app.serverSummary
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
                    if (state.parent != null) {
                        IconButton(onClick = vm::navigateUp) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "上一级")
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
        ) {
            Breadcrumb(
                path = state.path,
                onNavigate = vm::load,
            )
            HorizontalDivider()
            if (state.scanning || (state.loading && state.entries.isNotEmpty())) {
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }

            when {
                state.loading -> LoadingView()
                state.error != null -> ErrorView(
                    message = state.error ?: "加载失败",
                    onRetry = vm::refresh,
                    onSecondary = onDisconnect,
                    secondaryLabel = "断开连接",
                )
                state.entries.isEmpty() -> EmptyView("此目录为空")
                else -> LazyColumn(modifier = Modifier.fillMaxSize()) {
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
                            coverUrl = entry.bookId?.let { app.libraryRepository.coverUrl(it, 240) },
                            onClick = { vm.onEntryClick(entry) },
                            onOpenAsGallery = { vm.openFolderAsGallery(entry.path) },
                            opening = state.opening,
                        )
                        HorizontalDivider()
                    }
                    if (state.hasMore) {
                        item(key = "load-more") {
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable { vm.loadMore() }
                                    .padding(16.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                if (state.loadingMore) {
                                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                                } else {
                                    Text("加载更多（${state.entries.size}/${state.total}）")
                                }
                            }
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
            .collect { max -> if (max > 0) scrollState.animateScrollTo(max) }
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
