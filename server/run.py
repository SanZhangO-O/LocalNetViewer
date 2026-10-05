"""LocalNetViewer 服务器启动入口（设计 4.10）。

用法::

    cd server
    python -m venv .venv && .venv\\Scripts\\activate    # Windows
    pip install -e .
    python run.py --config config.yaml

启动时会打印：书库与书籍数、全部可用地址（多网卡 / VPN 场景标注网卡类型）、
是否启用令牌，以及 Windows 防火墙放行命令。
"""

from __future__ import annotations

import argparse
import logging
import platform
import sys
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
if str(BASE_DIR) not in sys.path:
    sys.path.insert(0, str(BASE_DIR))


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="run.py",
        description="LocalNetViewer 局域网电子书阅读服务器",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="配置文件路径（默认 server/config.yaml）",
    )
    parser.add_argument("--host", default=None, help="覆盖监听地址（默认取配置）")
    parser.add_argument("--port", type=int, default=None, help="覆盖监听端口（默认取配置）")
    parser.add_argument(
        "--no-scan",
        action="store_true",
        help="启动时不等待首次书库扫描（直接对外服务，索引在后台补齐）",
    )
    parser.add_argument(
        "--reload",
        action="store_true",
        help="开发模式：代码变更自动重载（该模式忽略 --config，使用默认 config.yaml）",
    )
    parser.add_argument(
        "--log-level",
        default="info",
        choices=["critical", "error", "warning", "info", "debug", "trace"],
        help="日志级别",
    )
    return parser.parse_args(argv)


def startup_banner(ctx, host: str, port: int) -> str:
    lines = [ctx.startup_report(), "", f"监听: {host}:{port}", "可用地址:"]
    addresses = _addresses()
    if not addresses:
        lines.append(f"  http://127.0.0.1:{port}   (仅本机，未检测到局域网地址)")
    for addr in addresses:
        lines.append(f"  http://{addr.url_host}:{port}   ({addr.label})")
    lines.extend(_security_warnings(ctx, host))
    if platform.system() == "Windows":
        lines.append("")
        lines.append("Windows 首次运行若手机连不上，请以管理员执行：")
        lines.append(f"  {_firewall_hint(port)}")
    lines.append("")
    lines.append("提示：仅建议在局域网使用，不要映射到公网端口。")
    return "\n".join(lines)


def _security_warnings(ctx, host: str) -> list:
    """对外监听且未配令牌时的醒目告警（安全默认见 config.ServerCfg.host）。"""
    loopback = host in ("127.0.0.1", "localhost", "::1") or host.startswith("127.")
    if loopback:
        return []
    token_on = bool((ctx.config.server.auth_token or "").strip())
    if token_on:
        return []
    return [
        "",
        "⚠️  安全警告：当前监听 " + host + "（局域网内所有设备可访问）且未启用访问令牌。",
        "   同一网络中的任何人都能浏览您的整个书库。建议在 config.yaml 中设置：",
        "     server:",
        "       auth_token: <自定义口令>",
        "   或将 host 改为 127.0.0.1（仅本机可用）。",
    ]


def _addresses():
    from app.netinfo import enumerate_addresses

    return enumerate_addresses()


def _firewall_hint(port: int) -> str:
    from app.netinfo import firewall_hint

    return firewall_hint(port)


def _configure_stdio() -> None:
    """让 stdout/stderr 在非 UTF-8 控制台（zh-CN 的 GBK、重定向到管道）下也能打印。

    启动横幅里含书库名与路径；只要有一个字符不在控制台代码页里（emoji、西里尔字母…），
    ``print`` 就会抛 UnicodeEncodeError —— 而且是在 uvicorn 启动**之前**，
    表现成"服务器起不来"，与真正的原因完全无关。
    """
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(errors="replace")
        except Exception:  # noqa: BLE001 - 尽力而为，失败也不该影响启动
            continue


def main(argv=None) -> int:
    args = parse_args(argv)
    _configure_stdio()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    logger = logging.getLogger("localnetviewer.run")

    try:
        import uvicorn
    except ImportError:  # pragma: no cover
        print("缺少依赖 uvicorn，请先执行: pip install -e .", file=sys.stderr)
        return 2

    from app.config import ConfigError, load_config
    from app.main import create_app

    try:
        cfg = load_config(args.config)
    except ConfigError as exc:
        print(f"配置错误：{exc}", file=sys.stderr)
        return 2

    if args.host:
        cfg.server.host = args.host
    if args.port:
        cfg.server.port = int(args.port)

    if args.reload:
        logger.warning("--reload 模式忽略 --config，使用默认 config.yaml")
        uvicorn.run(
            "app.main:create_app",
            factory=True,
            host=cfg.server.host,
            port=cfg.server.port,
            reload=True,
            log_level=args.log_level,
        )
        return 0

    # background_scan 必须与 --no-scan 一致：create_app(background_scan=False)
    # 会同时关掉 lifespan 里的后台扫描，于是 --no-scan 变成"永远不扫描"，
    # 而它的帮助文本承诺的是"索引在后台补齐"。
    # lazy 模式（library.scan_mode=lazy，默认）不做任何全量扫描：索引由 /files
    # 列出目录时按需增量补齐，故 background_scan 恒为 False。
    lazy_scan = cfg.library.scan_mode == "lazy"
    app = create_app(cfg, background_scan=(not lazy_scan) and bool(args.no_scan))
    ctx = app.state.ctx

    if not lazy_scan and not args.no_scan:
        # 启动前同步扫描一次，让启动信息里的书籍数准确（增量，第二次启动很快）
        try:
            ctx.library.scan_blocking()
        except Exception as exc:  # noqa: BLE001 - 扫描失败不应阻止服务启动
            logger.warning("首次书库扫描失败：%s", exc)

    # flush=True：stdout 被重定向（写日志文件 / 交给服务管理器）时是块缓冲的，
    # 不刷新的话整段启动信息要等到进程退出才出现 —— 而它正是 run.py 的主要输出
    print(startup_banner(ctx, cfg.server.host, cfg.server.port), flush=True)
    print(flush=True)

    uvicorn.run(
        app,
        host=cfg.server.host,
        port=cfg.server.port,
        log_level=args.log_level,
        workers=1,  # 单进程部署，内存缓存天然一致（设计 4.9）
        timeout_keep_alive=30,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
