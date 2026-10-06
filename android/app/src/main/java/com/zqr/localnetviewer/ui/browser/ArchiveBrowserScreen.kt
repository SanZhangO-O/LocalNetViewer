package com.zqr.localnetviewer.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.filled.Collections
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.Refresh
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import coil3.compose.AsyncImage
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.net.dto.ArchiveEntryItemDto
import com.zqr.localnetviewer.ui.common.EmptyView
import com.zqr.localnetviewer.ui.common.ErrorView
import com.zqr.localnetviewer.ui.common.LoadingView
import com.zqr.localnetviewer.ui.common.formatSize

/**
 * 压缩包内容浏览页（设计 7.6）：把压缩包当虚拟目录浏览。
 *
 * - 目录条目下钻；书籍/图片条目注册为嵌套书后进入阅读器（进度独立）；
 * - 目录可"作为图集打开"（该前缀下的图片即页，覆盖包内多话漫画）；
 * - 返回键先在包内逐级上溯，到包根后再退出本页。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ArchiveBrowserScreen(
    bookId: String,
    onOpenBook: (String) -> Unit,
    onBack: () -> Unit,
    overrideViewModel: ArchiveBrowserViewModel? = null,
) {
    val app = App.from(androidx.compose.ui.platform.LocalContext.current)
    val vm = overrideViewModel ?: viewModel<ArchiveBrowserViewModel>(
        key = bookId,
        factory = viewModelFactory {
            initializer { ArchiveBrowserViewModel(app, bookId) }
        },
    )
    val state by vm.state.collectAsStateWithLifecycle()
    val openBook by vm.openBook.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }

    androidx.activity.compose.BackHandler {
        if (state.canNavigateUp) vm.navigateUp() else onBack()
    }

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
                            text = state.entry.substringAfterLast('/').ifEmpty { "压缩包内容" },
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        val summary = state.summary
                        val subtitle = if (summary.entryCount > 0) {
                            "压缩包 · ${summary.entryCount} 项 · 图片 ${summary.imageCount}"
                        } else {
                            "压缩包内容"
                        }
                        Text(
                            text = subtitle,
                            style = MaterialTheme.typography.labelSmall,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                },
                navigationIcon = {
                    IconButton(
                        onClick = { if (state.canNavigateUp) vm.navigateUp() else onBack() }
                    ) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                actions = {
                    IconButton(onClick = vm::refresh) {
                        Icon(Icons.Default.Refresh, contentDescription = "刷新")
                    }
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
            if (state.loading && state.entries.isNotEmpty()) {
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }
            when {
                state.loading && state.entries.isEmpty() -> LoadingView()
                state.error != null -> ErrorView(message = state.error ?: "加载失败", onRetry = vm::refresh)
                state.entries.isEmpty() -> EmptyView("压缩包内没有可见条目")
                else -> LazyColumn(modifier = Modifier.fillMaxSize()) {
                    itemsIndexed(state.entries, key = { i, item -> "${item.path}#$i" }) { _, item ->
                        ArchiveEntryRow(
                            item = item,
                            previewUrl = if (item.isImage) vm.previewUrl(item) else null,
                            onClick = { vm.onEntryClick(item) },
                            onOpenAsGallery = { vm.openAsGallery(item) },
                            opening = state.openingPath == item.path,
                        )
                        HorizontalDivider()
                    }
                    if (state.hasMore) {
                        item(key = "load-more") {
                            TextButton(
                                onClick = vm::loadMore,
                                modifier = Modifier.fillMaxWidth(),
                            ) {
                                Text("加载更多（${state.entries.size} / ${state.total}）")
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ArchiveEntryRow(
    item: ArchiveEntryItemDto,
    previewUrl: String?,
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
        ArchiveEntryLeading(item = item, previewUrl = previewUrl)

        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = item.name,
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            val description = when {
                item.isDir -> buildString {
                    val count = item.gallery?.imageCount ?: 0
                    append(if (count > 0) "文件夹 · $count 张图片" else "文件夹")
                }
                item.isImage -> "图片"
                item.isBook -> buildString {
                    append(item.format?.uppercase() ?: "未知格式")
                    val size = formatSize(item.size)
                    if (size.isNotEmpty()) append(" · ").append(size)
                }
                else -> buildString {
                    val size = formatSize(item.size)
                    if (size.isNotEmpty()) append(size)
                }
            }
            val blocked = !item.isDir && !item.openable
            if (description.isNotEmpty() || blocked) {
                Text(
                    text = if (blocked) "$description · ${item.reason ?: "不可打开"}" else description,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (blocked) {
                        MaterialTheme.colorScheme.error
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
        }

        if (opening) {
            LinearProgressIndicator(modifier = Modifier.width(28.dp))
        }
        if (item.isDir && item.gallery?.canOpenAsBook == true) {
            TextButton(onClick = onOpenAsGallery) { Text("作为图集打开") }
        }
    }
}

@Composable
private fun ArchiveEntryLeading(item: ArchiveEntryItemDto, previewUrl: String?) {
    val modifier = Modifier
        .size(width = 44.dp, height = 58.dp)
        .clip(RoundedCornerShape(4.dp))
    when {
        item.isImage && previewUrl != null -> {
            AsyncImage(
                model = previewUrl,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = modifier,
            )
        }
        item.isDir -> ArchiveIconBox(Icons.Default.Folder)
        item.isImage -> ArchiveIconBox(Icons.Default.Image)
        item.isBook -> ArchiveIconBox(Icons.Default.Collections)
        else -> ArchiveIconBox(Icons.AutoMirrored.Filled.InsertDriveFile)
    }
}

@Composable
private fun ArchiveIconBox(icon: ImageVector) {
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
