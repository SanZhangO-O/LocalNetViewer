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
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zqr.localnetviewer.data.net.dto.ChapterInfoDto

/**
 * 目录抽屉（设计 6.3）：点击章节跳到 `start_page`。
 *
 * 高亮按**章节下标**判定：按标题字符串比较时，同名章节会一起变粗，标题为空时
 * 又永远不会高亮。打开时还会滚动到当前章，千章网文不需要手动翻找。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TocSheet(
    chapters: List<ChapterInfoDto>,
    currentChapterIndex: Int,
    currentChapterTitle: String,
    onSelect: (Int) -> Unit,
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

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Text(
                text = "目录（${chapters.size} 章）",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
            if (chapters.isEmpty()) {
                Text(
                    text = "本书没有章节目录",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp),
                )
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
            Spacer(modifier = Modifier.padding(bottom = 16.dp))
        }
    }
}
