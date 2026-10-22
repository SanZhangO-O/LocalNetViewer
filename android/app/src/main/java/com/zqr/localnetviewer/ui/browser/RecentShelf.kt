package com.zqr.localnetviewer.ui.browser

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import com.zqr.localnetviewer.data.prefs.RecentBook
import kotlin.math.roundToInt

/**
 * "继续阅读"横排（书库根目录 / 当前目录顶部）。
 *
 * 反复打开 App 的动作分布里，"接着看昨晚那本"远多于"找一本新书"，而原来的
 * 入口是：连接 → 书库根 → 一层层点目录 → 找到文件。这里把本地进度直接摆到
 * 第一屏，点一下进书；书架上没有进度时不占任何高度。
 *
 * 在子目录里只显示该书架下的书（见 [recentInPath]）：读者既然已经走到"小说/"
 * 里，再给他看"技术/"里最近读的书只是噪音。
 */
@Composable
fun RecentReadingShelf(
    recent: List<RecentBook>,
    coverUrl: (String) -> String,
    onOpen: (String) -> Unit,
) {
    if (recent.isEmpty()) return
    Column(modifier = Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 2.dp)) {
        Text(
            text = "继续阅读",
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(horizontal = 16.dp),
        )
        Spacer(Modifier.height(6.dp))
        LazyRow(
            contentPadding = PaddingValues(horizontal = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            items(recent, key = { it.bookId }) { book ->
                RecentCard(
                    book = book,
                    coverUrl = coverUrl(book.bookId),
                    onClick = { onOpen(book.bookId) },
                )
            }
        }
    }
}

@Composable
private fun RecentCard(book: RecentBook, coverUrl: String, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .width(84.dp)
            .clip(RoundedCornerShape(6.dp))
            .clickable(onClick = onClick)
            .padding(4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        AsyncImage(
            model = coverUrl,
            contentDescription = null,
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .size(width = 64.dp, height = 88.dp)
                .clip(RoundedCornerShape(4.dp)),
        )
        Spacer(Modifier.height(4.dp))
        Text(
            text = book.displayTitle,
            style = MaterialTheme.typography.labelMedium,
            maxLines = 2,
            minLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
        )
        val fraction = book.fraction
        if (fraction != null) {
            LinearProgressIndicator(
                progress = { fraction },
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 4.dp)
                    .height(2.dp),
            )
            Text(
                text = "已读 ${(fraction * 100).roundToInt()}%",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        } else if (book.progress.page > 1) {
            val total = book.progress.totalPages
            Text(
                text = if (total > 0) {
                    "第 ${book.progress.page} / $total 页"
                } else {
                    "第 ${book.progress.page} 页"
                },
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
    }
}

/**
 * 当前目录下的最近阅读：根目录给全部，子目录只给该子树（按 vpath 前缀）。
 *
 * 读者走到某个书架里之后，别处最近读的书只是噪音。
 */
fun recentInPath(recent: List<RecentBook>, path: String): List<RecentBook> {
    val base = path.trimEnd('/')
    if (base.isEmpty()) return recent
    val prefix = "$base/"
    return recent.filter { it.progress.vpath.startsWith(prefix) }
}
