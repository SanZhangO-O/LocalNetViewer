package com.zqr.localnetviewer.ui.nav

import android.net.Uri
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.prefs.AppSettings
import com.zqr.localnetviewer.ui.browser.BrowserScreen
import com.zqr.localnetviewer.ui.connect.ConnectScreen
import com.zqr.localnetviewer.ui.reader.ReaderScreen

/** 路由表（设计 6.2：连接页 / 浏览页 / 阅读页）。 */
object Routes {
    const val CONNECT = "connect"
    const val BROWSER = "browser"
    const val READER = "reader/{bookId}"
    const val ARG_BOOK_ID = "bookId"

    // bookId URL 编码：图集 book_id 可能含 "/" 等路径字符，不编码会被路由拆参
    fun reader(bookId: String): String = "reader/${Uri.encode(bookId)}"
}

@Composable
fun AppNavHost() {
    val navController = rememberNavController()
    val app = App.from(LocalContext.current)
    // 页面间过渡跟随界面预设（设计 6.3）：Navigation 默认是 700ms 交叉淡入淡出，
    // 不接入预设的话，墨水屏（默认预设）上每次连接/打开书/返回都会触发多次灰阶
    // 全刷与残影。eink 预设直接落位（单次刷新）；lcd/custom 开动画时保留原默认渐变。
    // DataStore 就绪前用 DEFAULT（eink）兜底，冷启动第一跳也不会有动画。
    val settings by app.settingsStore.settingsFlow.collectAsStateWithLifecycle(
        initialValue = AppSettings.DEFAULT
    )
    val animate = settings.animateTransitions
    NavHost(
        navController = navController,
        startDestination = Routes.CONNECT,
        enterTransition = { if (animate) fadeIn(tween(700)) else EnterTransition.None },
        exitTransition = { if (animate) fadeOut(tween(700)) else ExitTransition.None },
        popEnterTransition = { if (animate) fadeIn(tween(700)) else EnterTransition.None },
        popExitTransition = { if (animate) fadeOut(tween(700)) else ExitTransition.None },
    ) {
        composable(Routes.CONNECT) {
            ConnectScreen(
                onConnected = {
                    navController.navigate(Routes.BROWSER) {
                        // 连接成功后不再回到连接页
                        popUpTo(Routes.CONNECT) { inclusive = true }
                        launchSingleTop = true
                    }
                }
            )
        }

        composable(Routes.BROWSER) {
            BrowserScreen(
                onOpenBook = { bookId -> navController.navigate(Routes.reader(bookId)) },
                onDisconnect = {
                    // 断开连接：清空概要与整个返回栈，回到连接页
                    // （不清空的话，重连失败时连接页/浏览页残留旧服务器的摘要）
                    app.updateServerSummary("")
                    navController.navigate(Routes.CONNECT) {
                        popUpTo(navController.graph.startDestinationId) { inclusive = true }
                        launchSingleTop = true
                    }
                },
            )
        }

        composable(
            route = Routes.READER,
            arguments = listOf(navArgument(Routes.ARG_BOOK_ID) { type = NavType.StringType }),
        ) { entry ->
            val bookId = entry.arguments?.getString(Routes.ARG_BOOK_ID).orEmpty()
            ReaderScreen(
                bookId = bookId,
                onBack = { navController.popBackStack() },
            )
        }
    }
}
