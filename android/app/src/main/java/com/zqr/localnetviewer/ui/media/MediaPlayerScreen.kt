package com.zqr.localnetviewer.ui.media

import android.net.Uri
import android.widget.MediaController
import android.widget.VideoView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.viewinterop.AndroidView

/**
 * 内嵌媒体播放页（M5 Phase 5）：音频/视频直接用系统 VideoView（MediaPlayer）播放；
 *
 * - 服务器 /media 支持 Range（206），拖动进度条不会整段下载；
 * - 音频（audioOnly）显示大图标占位，播放控制条仍然可用；
 * - 不引入 Media3/ExoPlayer：系统解码器足够覆盖 mp4/mp3 等常见格式，也少一个重依赖。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MediaPlayerScreen(
    url: String,
    title: String,
    audioOnly: Boolean,
    onBack: () -> Unit,
) {
    val view = LocalView.current
    val playerRef = remember { arrayOfNulls<VideoView>(1) }

    DisposableEffect(Unit) {
        view.keepScreenOn = true
        onDispose {
            view.keepScreenOn = false
            playerRef[0]?.stopPlayback()
            playerRef[0] = null
        }
    }

    fun stopPlayback() {
        playerRef[0]?.stopPlayback()
    }

    BackHandler {
        stopPlayback()
        onBack()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(text = title, maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
                navigationIcon = {
                    IconButton(onClick = {
                        stopPlayback()
                        onBack()
                    }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
            )
        }
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding),
            contentAlignment = Alignment.Center,
        ) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { ctx ->
                    VideoView(ctx).apply {
                        setVideoURI(Uri.parse(url))
                        val controller = MediaController(ctx)
                        controller.setAnchorView(this)
                        setMediaController(controller)
                        setOnPreparedListener { media ->
                            media.isLooping = false
                            start()
                        }
                        setOnErrorListener { _, _, _ ->
                            // 解码失败：交给系统播放器行为（保持界面可返回），
                            // 真正的错误原因由服务器日志/网络层给出
                            true
                        }
                        playerRef[0] = this
                    }
                },
            )
            if (audioOnly) {
                Icon(
                    imageVector = Icons.Default.GraphicEq,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.fillMaxSize(0.35f),
                )
            }
        }
    }
}
