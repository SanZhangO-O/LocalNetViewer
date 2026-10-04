#pragma once
// HTTP 服务层（cpp-httplib；对应 Python app/main.py + api/routes_*.py）
#include "lnv/context.h"
#include <httplib.h>
#include <memory>
#include <string>

namespace lnv {

// 创建并注册全部路由（前缀 /api/v1）。返回的 Server 由调用方 listen。
std::unique_ptr<httplib::Server> create_server(AppContext& ctx);

// 阻塞运行 HTTP 服务；listen 失败时向 stderr 打印并 exit(1)。
int run_http(AppContext& ctx, const std::string& host, int port);

// 优雅退出：供控制台 Ctrl 处理器调用，内部通知 run_http 停止 listen。
void request_http_stop();

}  // namespace lnv
