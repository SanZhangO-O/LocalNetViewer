package com.zqr.localnetviewer.ui.nav

import android.net.Uri
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.zqr.localnetviewer.App
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
    NavHost(navController = navController, startDestination = Routes.CONNECT) {
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
