package com.zqr.localnetviewer.ui.connect

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.KeyboardOptions
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.zqr.localnetviewer.App
import com.zqr.localnetviewer.data.prefs.ServerAddress

/** 连接页（设计 6.3）：地址 + 令牌 + 历史地址列表。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectScreen(
    onConnected: () -> Unit,
    overrideViewModel: ConnectViewModel? = null,
) {
    val context = LocalContext.current
    // 注意：参数名不能叫 viewModel，否则会遮蔽 androidx.lifecycle.viewmodel.compose.viewModel()
    val vm = overrideViewModel ?: viewModel<ConnectViewModel>(
        factory = viewModelFactory { initializer { ConnectViewModel(App.from(context)) } }
    )
    val state by vm.state.collectAsStateWithLifecycle()

    LaunchedEffect(state.connected) {
        if (state.connected) {
            vm.consumeConnected()
            onConnected()
        }
    }

    Scaffold(
        topBar = { TopAppBar(title = { Text("连接服务器") }) }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            OutlinedTextField(
                value = state.address,
                onValueChange = vm::onAddressChange,
                label = { Text("服务器地址") },
                placeholder = { Text("192.168.1.10 或 192.168.1.10:8765") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = state.token,
                onValueChange = vm::onTokenChange,
                label = { Text("访问令牌（服务器未启用则留空）") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth(),
            )

            Button(
                onClick = { vm.connect() },
                enabled = !state.connecting,
                modifier = Modifier.fillMaxWidth(),
            ) {
                if (state.connecting) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(18.dp),
                        strokeWidth = 2.dp,
                        color = MaterialTheme.colorScheme.onPrimary,
                    )
                    Spacer(Modifier.width(8.dp))
                }
                Text(if (state.connecting) "连接中…" else "连接")
            }

            state.error?.let { message ->
                Card {
                    Column(Modifier.padding(12.dp)) {
                        Text(
                            text = message,
                            color = MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }

            if (state.serverName.isNotEmpty()) {
                Card {
                    Row(
                        modifier = Modifier.padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Default.Dns, contentDescription = null)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(
                                text = "${state.serverName} v${state.serverVersion}",
                                style = MaterialTheme.typography.titleMedium,
                            )
                            Text(
                                text = state.librarySummary,
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
            }

            Spacer(Modifier.height(4.dp))
            Text("历史地址", style = MaterialTheme.typography.titleSmall)

            if (state.history.isEmpty()) {
                Text(
                    text = "暂无历史地址",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                state.history.forEach { item ->
                    HistoryRow(
                        item = item,
                        onClick = { vm.useHistory(item) },
                        onDelete = { vm.forget(item.url) },
                    )
                }
            }

            Spacer(Modifier.height(8.dp))
            Text(
                text = "提示：先在 PC 上执行 server/run.py，启动信息会打印可用的 " +
                    "http://IP:8765 地址；手机需与 PC 处于同一局域网。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun HistoryRow(
    item: ServerAddress,
    onClick: () -> Unit,
    onDelete: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable(onClick = onClick)
                .padding(start = 12.dp, top = 4.dp, bottom = 4.dp, end = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(text = item.name.ifEmpty { item.url }, style = MaterialTheme.typography.bodyLarge)
                Text(
                    text = item.url,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            IconButton(onClick = onDelete) {
                Icon(Icons.Default.Delete, contentDescription = "删除")
            }
        }
    }
}
