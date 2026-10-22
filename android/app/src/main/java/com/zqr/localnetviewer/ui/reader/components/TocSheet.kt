package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zqr.localnetviewer.data.net.dto.ChapterInfoDto
import com.zqr.localnetviewer.data.prefs.Bookmark

/**
 * 目录抽屉（设计 6.3）：点击章节跳到 `start_page`。
 *
 * 高亮按**章节下标**判定：按标题字符串比较时，同名章节会一起变粗，标题为空时
 * 又永远不会高亮。打开时还会滚动到当前章，千章网文不需要手动翻找。
 *
 * v0.9 起同一个抽屉里放"书签"页：两者都是"跳到哪里去"，分两个入口只会让
 * 顶栏越来越挤；书签列表按页码升序，点一下走锚点定位（改字号后仍准确）。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TocSheet(
    chapters: List<ChapterInfoDto>,
    currentChapterIndex: Int,
    currentChapterTitle: String,
    bookmarks: List<Bookmark>,
    onSelect: (Int) -> Unit,
    onJumpBookmark: (Bookmark) -> Unit,
    onDeleteBookmark: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val currentIndex = remember(chapters, currentChapterIndex, currentChapterTitle) {
        when {
            chapters.any { it.index == currentChapterIndex } -> currentChapterIndex
            currentChapterTitle.isNotEmpty() ->
                chapters.indexOfFirst { it.title == currentChapterTitle }
            else -> -1
        }
    }
    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = currentIndex.coerceAtLeast(0)
    )
    // 目录数据可能在弹层打开后才补齐（章节起始页由 /layout 返回）
    LaunchedEffect(currentIndex) {
        if (currentIndex > 0) listState.scrollToItem(currentIndex)
    }

    // 没有目录（纯图片书 / TXT 无章节）但有书签时，直接落在书签页
    var tab by remember { mutableIntStateOf(if (chapters.isEmpty() && bookmarks.isNotEmpty()) 1 else 0) }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(modifier = Modifier.fillMaxWidth()) {
            TabRow(selectedTabIndex = tab) {
                Tab(
                    selected = tab == 0,
                    onClick = { tab = 0 },
                    text = { Text("目录（${chapters.size}）") },
                )
                Tab(
                    selected = tab == 1,
                    onClick = { tab = 1 },
                    text = { Text("书签（${bookmarks.size}）") },
                )
            }
            if (tab == 0) {
                if (chapters.isEmpty()) {
                    EmptyHint("本书没有章节目录")
                } else {
                    LazyColumn(
                        state = listState,
                        // 横屏 / 小屏自适应：按可用高度的比例收，而不是固定 dp
                        modifier = Modifier.heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.7f).dp),
                    ) {
                        itemsIndexed(chapters, key = { _, chapter -> chapter.index }) { _, chapter ->
                            val selected = chapter.index == currentIndex
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .background(
                                        if (selected) {
                                            MaterialTheme.colorScheme.secondaryContainer
                                        } else {
                                            MaterialTheme.colorScheme.surface
                                        }
                                    )
                                    .clickable { onSelect(chapter.index) }
                                    .padding(horizontal = 20.dp, vertical = 12.dp),
                            ) {
                                Text(
                                    text = chapter.title.ifEmpty { "第 ${chapter.index + 1} 章" },
                                    style = MaterialTheme.typography.bodyLarge,
                                    fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                                    maxLines = 2,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f),
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = chapter.startPage?.let { "第 $it 页" } ?: "",
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
            } else {
                if (bookmarks.isEmpty()) {
                    EmptyHint("还没有书签：阅读时点顶栏的书签图标收藏当前页")
                } else {
                    LazyColumn(
                        modifier = Modifier.heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.7f).dp),
                    ) {
                        items(bookmarks, key = { it.id }) { bookmark ->
                            BookmarkRow(
                                bookmark = bookmark,
                                onClick = { onJumpBookmark(bookmark) },
                                onDelete = { onDeleteBookmark(bookmark.id) },
                            )
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.padding(bottom = 16.dp))
        }
    }
}

@Composable
private fun EmptyHint(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp),
    )
}

@Composable
private fun BookmarkRow(bookmark: Bookmark, onClick: () -> Unit, onDelete: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(start = 20.dp, end = 4.dp, top = 10.dp, bottom = 10.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = buildString {
                    append("第 ${bookmark.page} 页")
                    if (bookmark.chapterTitle.isNotEmpty()) {
                        append(" · ").append(bookmark.chapterTitle)
                    }
                },
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (bookmark.note.isNotEmpty()) {
                Text(
                    text = bookmark.note,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        IconButton(onClick = onDelete) {
            Icon(Icons.Default.Delete, contentDescription = "删除书签")
        }
    }
}
