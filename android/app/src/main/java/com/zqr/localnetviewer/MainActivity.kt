package com.zqr.localnetviewer

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import com.zqr.localnetviewer.ui.nav.AppNavHost
import com.zqr.localnetviewer.ui.theme.LocalNetViewerTheme

/**
 * 唯一 Activity（设计 6.2）：内容是 Navigation Compose 的三个页面。
 *
 * `enableEdgeToEdge` + 阅读页自行处理 WindowInsets，保证正文不被状态栏/手势条遮挡。
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            // dynamicColor 默认关闭：见 ui/theme/Theme.kt 的说明（阅读类应用要稳定观感）
            LocalNetViewerTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    AppNavHost()
                }
            }
        }
    }
}
