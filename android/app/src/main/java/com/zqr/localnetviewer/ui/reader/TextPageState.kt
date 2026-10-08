package com.zqr.localnetviewer.ui.reader

import com.zqr.localnetviewer.data.net.dto.PageJsonDto

/**
 * 文字直发模式下单页 JSON 的取回状态（设计 3.2）。
 *
 * 页面内容在 ViewModel 里按 `页号 → 状态` 缓存；[Loading] 与 [Failed] 都由图
 * 组件呈现（加载指示 / 重试占位），与 PNG 模式由 Coil 汇报状态的行为对齐。
 */
sealed interface TextPageState {

    data object Loading : TextPageState

    data class Ready(val page: PageJsonDto) : TextPageState

    data class Failed(val message: String) : TextPageState
}
