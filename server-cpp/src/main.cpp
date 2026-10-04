// LocalNetViewer C++ server 入口（对应 Python server/run.py）
// 启动顺序：CLI → 加载配置（失败 exit 2）→ AppContext → 同步扫描 → 横幅 → run_http 阻塞。
#include <cstdio>
#include <windows.h>

#include "lnv/config.h"
#include "lnv/http/server.h"
#include "lnv/netinfo.h"

namespace {

// Ctrl+C / 关窗 → 优雅退出（内部 stop listen，使 run_http 返回后走 shutdown flush）
BOOL WINAPI console_ctrl_handler(DWORD type) {
    (void)type;
    lnv::request_http_stop();
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);  // 中文输出不乱码

    std::optional<std::string> config_path;
    std::optional<std::string> host;
    std::optional<int> port;
    bool no_scan = false;
    // CLI：--config / --host / --port / --no-scan / --log-level
    // 与 argparse 对齐（审查 L17）：未知选项、缺值、非整数端口、非法 log-level
    // 都要报错退出（Python 退出码 2），不能静默忽略或 atoi 成 0。
    auto usage_error = [](const std::string& msg) {
        std::fprintf(stderr, "参数错误：%s\n", msg.c_str());
        std::fprintf(stderr,
                     "用法: lnvserver [--config PATH] [--host HOST] [--port PORT] "
                     "[--no-scan] [--log-level LEVEL]\n");
        return 2;
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool have_next = i + 1 < argc;
        if (a == "--config" || a == "--host" || a == "--port" || a == "--log-level") {
            if (!have_next) return usage_error(a + " 缺少取值");
        }
        if (a == "--config") config_path = argv[++i];
        else if (a == "--host") host = argv[++i];
        else if (a == "--port") {
            const std::string v = argv[++i];
            try {
                size_t pos = 0;
                int p = std::stoi(v, &pos);
                if (pos != v.size() || p < 1 || p > 65535) return usage_error("--port 取值非法: " + v);
                port = p;
            } catch (...) {
                return usage_error("--port 取值非法: " + v);
            }
        } else if (a == "--no-scan") no_scan = true;
        else if (a == "--log-level") {
            const std::string v = argv[++i];
            static const char* const kLevels[] = {"critical", "error", "warning",
                                                  "info", "debug", "trace"};
            bool ok = false;
            for (const char* l : kLevels) if (v == l) { ok = true; break; }
            if (!ok) return usage_error("--log-level 取值非法: " + v);
        } else if (a == "--reload") { /* 开发用，忽略 */ }
        else if (a == "-h" || a == "--help") {
            std::printf("用法: lnvserver [--config PATH] [--host HOST] [--port PORT] "
                        "[--no-scan] [--log-level LEVEL]\n");
            return 0;
        } else {
            return usage_error("未知选项: " + a);
        }
    }

    // 1) 加载配置
    lnv::AppConfig cfg;
    try {
        cfg = lnv::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "配置错误：%s\n", e.what());
        return 2;
    }
    if (host && !host->empty()) cfg.server.host = *host;
    if (port && *port) cfg.server.port = *port;

    // 2) 上下文（ensure_dirs、缓存、服务对象等）
    // background_scan 必须与 --no-scan 一致（对应 run.py: create_app(cfg,
    // background_scan=bool(args.no_scan))）：--no-scan 的语义是"不阻塞启动、后台补扫"，
    // 传 false 会让它变成"永远不扫描"，新放进书库的书在重启前都看不见。
    auto ctx = lnv::AppContext::create(std::move(cfg), no_scan);
    if (!ctx) {
        std::fprintf(stderr, "初始化失败\n");
        return 2;
    }

    // 3) 非 --no-scan：启动时同步全量扫描一次；失败仅警告继续
    if (!no_scan) {
        try {
            ctx->library->scan_blocking();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "警告：启动扫描失败：%s\n", e.what());
        }
    }

    // 4) 启动横幅
    std::printf("==============================================\n");
    std::printf("%s\n", ctx->startup_report().c_str());
    std::printf("监听：%s:%d\n", ctx->cfg.server.host.c_str(), ctx->cfg.server.port);
    // 网卡地址枚举；空则 127.0.0.1 兜底（仅本机）
    auto addrs = lnv::enumerate_addresses();
    if (addrs.empty()) {
        std::printf("本机访问：http://127.0.0.1:%d（仅本机）\n", ctx->cfg.server.port);
    } else {
        for (const auto& a : addrs)
            std::printf("访问：http://%s:%d  %s\n", a.url_host.c_str(),
                        ctx->cfg.server.port, a.label.c_str());
    }
    // host 非回环且未配令牌 → 醒目安全警告
    const std::string& h = ctx->cfg.server.host;
    const bool loopback =
        h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "0.0.0.0";
    if (!loopback && !ctx->cfg.auth_required()) {
        std::fprintf(stderr,
                     "\n*** 安全警告：监听地址 %s 非本机回环，且未配置 auth_token，"
                     "局域网内任何人可访问！建议在 config.yaml 配置 server.auth_token ***\n\n",
                     h.c_str());
    }
    std::printf("防火墙放行提示：%s\n", lnv::firewall_hint(ctx->cfg.server.port).c_str());
    std::printf("==============================================\n");
    std::fflush(stdout);

    // 5) Ctrl 处理器 + 阻塞运行；退出时 flush 索引/进度
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    int rc = lnv::run_http(*ctx, ctx->cfg.server.host, ctx->cfg.server.port);
    ctx->shutdown();
    return rc;
}
