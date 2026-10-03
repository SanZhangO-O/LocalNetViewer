package com.zqr.localnetviewer.ui.nav

import androidx.compose.runtime.Composable
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.zqr.localnetviewer.ui.browser.BrowserScreen
import com.zqr.localnetviewer.ui.connect.ConnectScreen
import com.zqr.localnetviewer.ui.reader.ReaderScreen

/** 路由表（设计 6.2：连接页 / 浏览页 / 阅读页）。 */
object Routes {
    const val CONNECT = "connect"
    const val BROWSER = "browser"
    const val READER = "reader/{bookId}"
    const val ARG_BOOK_ID = "bookId"

    fun reader(bookId: String): String = "reader/$bookId"
}

@Composable
fun AppNavHost() {
    val navController = rememberNavController()
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
                    // 断开连接：清空整个返回栈，回到连接页
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
