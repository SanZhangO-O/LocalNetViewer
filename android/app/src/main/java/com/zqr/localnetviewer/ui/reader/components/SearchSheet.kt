package com.zqr.localnetviewer.ui.reader.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zqr.localnetviewer.data.net.dto.SearchResultDto

/**
 * 全文搜索（设计 5.3 / M3）：结果直接带页码，点击跳页。
 *
 * 服务器在解析结果上搜索（TXT / EPUB），命中位置用 anchor 表示，带 layout_id 时给出页码。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SearchSheet(
    initialQuery: String,
    searching: Boolean,
    results: List<SearchResultDto>,
    limited: Boolean,
    onSearch: (String) -> Unit,
    onOpenResult: (SearchResultDto) -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var query by remember(initialQuery) { mutableStateOf(initialQuery) }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp)) {
            Text("全文搜索", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                label = { Text("搜索词") },
                singleLine = true,
                trailingIcon = {
                    IconButton(onClick = { onSearch(query) }) {
                        Icon(Icons.Default.Search, contentDescription = "搜索")
                    }
                },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { onSearch(query) }),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp),
            )

            Spacer(modifier = Modifier.padding(top = 4.dp))

            when {
                searching -> Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 16.dp),
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CircularProgressIndicator()
                }
                results.isEmpty() -> Text(
                    text = "输入关键词后开始搜索",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 12.dp),
                )
                // key 里带上下标：服务器的 (chapter, block, char) 理论上唯一，
                // 但 LazyColumn 遇到重复 key 会直接崩，多一个下标是零成本的保险
                else -> LazyColumn(
                    // 横屏 / 小屏自适应：按可用高度的比例收
                    modifier = Modifier.heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.55f).dp)
                ) {
                    itemsIndexed(
                        results,
                        key = { index, item ->
                            "$index-${item.chapterIndex}-${item.block}-${item.char}"
                        },
                    ) { _, item ->
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onOpenResult(item) }
                                .padding(vertical = 10.dp)
                        ) {
                            Text(
                                text = item.snippet,
                                style = MaterialTheme.typography.bodyMedium,
                                maxLines = 3,
                                overflow = TextOverflow.Ellipsis,
                            )
                            Row {
                                Text(
                                    text = item.chapterTitle.ifEmpty { "第 ${item.chapterIndex + 1} 章" },
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = item.page?.let { "第 $it 页" } ?: "暂无页码",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
            }

            if (limited) {
                Text(
                    text = "结果较多，仅显示前 ${results.size} 条",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(modifier = Modifier.padding(bottom = 20.dp))
        }
    }
}
