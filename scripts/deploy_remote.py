#!/usr/bin/env python3
"""把 server/ 远程部署到 Linux 主机（SSH）并启动 systemd 服务。

本脚本固化的是 2026-10-25 对 Orange Pi Zero 3（192.168.0.102）的实际部署流程：
打包当前工作区的 server/ → 上传 → 安装系统依赖（python3-venv、中文字体）→
创建 venv 并 pip install -e → 写入 config.yaml / systemd unit → 保证书库所在
磁盘持久挂载（fstab，nofail）→ 启动并做本机 HTTP 冒烟。

依赖：Python 3.9+、paramiko（`pip install paramiko`）。目标主机需可 SSH 登录且
该用户有 sudo 权限（sudo 密码默认与 SSH 密码相同，可用 --sudo-password 覆盖）。

示例（PowerShell）：
    $env:LNV_SSH_PASSWORD = "orangepi"
    python scripts/deploy_remote.py --host 192.168.0.102 --user orangepi `
        --library /mnt/usbdrive/book --font NotoSansCJK-Regular

重复执行是幂等的：代码会重新解压，venv / 缓存（远程 <remote-dir>/cache）保留。
注意：每次都会按命令行参数重新生成 config.yaml 与 systemd unit，手改会被覆盖。
"""

from __future__ import annotations

import argparse
import codecs
import getpass
import json
import os
import shlex
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

try:
    import paramiko
except ImportError:  # pragma: no cover - 明确的环境提示
    sys.exit("缺少 paramiko：请先执行 `pip install paramiko`（本脚本只在部署机需要）。")

REPO_ROOT = Path(__file__).resolve().parent.parent
SERVER_DIR = REPO_ROOT / "server"

# 不进入部署包：只排除 server/ 顶层的运行期目录（注意 server/app/cache 是源码！）
TOP_EXCLUDE_DIRS = {".git", ".venv", "venv", "cache", "tests"}
ANY_EXCLUDE_DIRS = {"__pycache__", ".pytest_cache"}
EXCLUDE_FILES = {"config.yaml"}
EXCLUDE_SUFFIXES = {".pyc", ".pyo"}

DEFAULT_PIP_INDEX = "https://mirrors.aliyun.com/pypi/simple/"


# --------------------------------------------------------------------------- SSH
class Remote:
    def __init__(self, client: paramiko.SSHClient, host: str):
        self.client = client
        self.host = host

    def _channel(self, command: str, timeout: int):
        chan = self.client.get_transport().open_session()
        chan.settimeout(30)
        chan.exec_command(command)
        return chan

    def capture(self, command: str, timeout: int = 120) -> tuple[int, str]:
        _stdin, stdout, stderr = self.client.exec_command(command, timeout=timeout)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        rc = stdout.channel.recv_exit_status()
        return rc, out + err

    def stream(self, command: str, *, timeout: int, stdin_line: str | None = None) -> int:
        """执行命令并实时打印输出；stdin_line 作为标准输入首行（用于 sudo 密码）。"""
        chan = self._channel(command, timeout)
        if stdin_line is not None:
            chan.sendall((stdin_line + "\n").encode("utf-8"))
        chan.shutdown_write()

        out_dec = codecs.getincrementaldecoder("utf-8")("replace")
        err_dec = codecs.getincrementaldecoder("utf-8")("replace")
        deadline = time.monotonic() + timeout
        while True:
            if chan.recv_ready():
                sys.stdout.write(out_dec.decode(chan.recv(65536)))
                sys.stdout.flush()
            if chan.recv_stderr_ready():
                sys.stderr.write(err_dec.decode(chan.recv_stderr(65536)))
                sys.stderr.flush()
            if (
                chan.exit_status_ready()
                and not chan.recv_ready()
                and not chan.recv_stderr_ready()
            ):
                break
            if time.monotonic() > deadline:
                chan.close()
                raise TimeoutError(f"远程命令超时（{timeout}s）：{command[:120]}")
            time.sleep(0.05)

        try:
            rc = chan.recv_exit_status()
        finally:
            chan.close()
        return rc


def connect(args, password: str | None) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        args.host,
        port=args.ssh_port,
        username=args.user,
        password=password or None,
        key_filename=args.key or None,
        timeout=20,
        banner_timeout=30,
        auth_timeout=30,
        look_for_keys=not password,
        allow_agent=not password,
    )
    client.get_transport().set_keepalive(30)
    return client


# ------------------------------------------------------------------------- 打包
def build_archive(target: Path) -> None:
    """把 server/ 打成 tar.gz（保留目录结构，解压时 --strip-components=1）。"""
    with tarfile.open(target, "w:gz") as tar:
        for root, dirs, files in os.walk(SERVER_DIR):
            rel_dir = Path(root).relative_to(SERVER_DIR)
            kept_dirs = []
            for name in dirs:
                if name in ANY_EXCLUDE_DIRS:
                    continue
                if not rel_dir.parts and name in TOP_EXCLUDE_DIRS:
                    continue
                kept_dirs.append(name)
            dirs[:] = sorted(kept_dirs)
            for name in sorted(files):
                if name in EXCLUDE_FILES and not rel_dir.parts:
                    continue
                if Path(name).suffix in EXCLUDE_SUFFIXES:
                    continue
                full = Path(root) / name
                arcname = full.relative_to(REPO_ROOT).as_posix()
                tar.add(full, arcname=arcname, recursive=False)


# ------------------------------------------------------------------------ 配置
def yaml_str(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def render_config(args, remote_dir: str) -> str:
    lines = [
        "server:",
        f"  name: {yaml_str('LocalNetViewer')}",
        f"  host: {yaml_str(args.bind)}",
        f"  port: {args.server_port}",
        f"  auth_token: {yaml_str(args.auth_token)}",
        "",
        "library:",
        "  roots:",
        f"    - name: {yaml_str(args.root_name)}",
        f"      path: {yaml_str(args.library)}",
        f"      password: {yaml_str(args.library_password)}",
        f"  scan_mode: {args.scan_mode}",
        "  watch: false",
        "  watch_debounce_s: 2.0",
        "  sniff_unknown: true",
        "",
    ]
    if args.font:
        lines += ["render:", f"  default_font: {yaml_str(args.font)}", ""]
    lines += [
        "cache:",
        f"  dir: {yaml_str(remote_dir + '/cache')}",
        f"  parsed_max_mb: {args.parsed_cache_mb}",
        f"  page_lru_bytes: {args.page_cache_mb * 1024 * 1024}",
        f"  page_lru_pages: {args.page_cache_pages}",
        f"  layout_memory_max: {args.layout_memory}",
        f"  layouts_max_mb: {args.layouts_cache_mb}",
        f"  doc_memory_max: {args.doc_memory}",
        "",
        "limits:",
        "  parse_timeout_s: 90",
        "",
    ]
    return "\n".join(lines)


def render_unit(args, remote_dir: str) -> str:
    library_unit = args.library.replace("\\", "\\\\").replace(" ", "\\x20")
    return f"""[Unit]
Description=LocalNetViewer Server
After=network-online.target
Wants=network-online.target
RequiresMountsFor={library_unit}

[Service]
Type=simple
User={args.user}
Group={args.group or args.user}
WorkingDirectory={remote_dir}/server
Environment=PYTHONUNBUFFERED=1
Environment=PYTHONIOENCODING=utf-8
ExecStart={remote_dir}/venv/bin/python run.py --config {remote_dir}/server/config.yaml
Restart=on-failure
RestartSec=5
TimeoutStopSec=20

[Install]
WantedBy=multi-user.target
"""


# ------------------------------------------------------------------- 远程脚本
INSTALL_TEMPLATE = r"""#!/usr/bin/env bash
set -euo pipefail

# 首行标准输入是 sudo 密码；之后每次 sudo 都从变量取，避免超时后卡住
IFS= read -r SUDOPW || true
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }

REMOTE_DIR=@@REMOTE_DIR@@
STAGING=@@STAGING@@
SERVICE=@@SERVICE@@
LIB=@@LIB@@
VENV="$REMOTE_DIR/venv"
SERVER="$REMOTE_DIR/server"

echo "== [1/7] system packages =="
if [ @@SKIP_APT@@ != "1" ]; then
  if ! command -v python3 >/dev/null 2>&1 || ! dpkg -s python3-venv >/dev/null 2>&1; then
    SUDO apt-get update -qq
    SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-venv python3-pip
  fi
  if ! command -v fc-list >/dev/null 2>&1 || ! fc-list :lang=zh 2>/dev/null | grep -q .; then
    SUDO apt-get update -qq
    SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq fonts-noto-cjk
  fi
  if ! command -v curl >/dev/null 2>&1; then
    SUDO apt-get update -qq
    SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq curl
  fi
fi

echo "== [2/7] python version =="
python3 - <<'PY'
import sys
if sys.version_info < (3, 11):
    sys.exit("need Python >= 3.11, got %s" % sys.version.split()[0])
print("python", sys.version.split()[0])
PY

echo "== [3/7] unpack server =="
rm -rf "$SERVER"
mkdir -p "$SERVER" "$REMOTE_DIR/cache"
tar xzf "$STAGING/server.tar.gz" --strip-components=1 -C "$SERVER"
install -m 600 "$STAGING/config.yaml" "$SERVER/config.yaml"

echo "== [4/7] venv + dependencies =="
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi
PIP_ARGS=(--timeout @@PIP_TIMEOUT@@ --retries 3)
if [ -n "@@PIP_INDEX@@" ]; then
  PIP_ARGS+=(--index-url @@PIP_INDEX@@)
fi
# 目标机 IPv6 常是"有地址但不通"：pip 会先试 AAAA 并卡住。检测失败时临时关掉
# 默认网卡的 IPv6，pip 结束后（或脚本退出时）恢复。
PIP_IPV6_IFACE=""
PIP_IPV6_OLD=""
if [ @@PIP_IPV4_FIX@@ = "1" ] && command -v curl >/dev/null 2>&1; then
  TEST_URL="${PIP_INDEX:-https://pypi.org/simple/}"
  if ! timeout 10 curl -6 -sS --max-time 8 -o /dev/null "$TEST_URL" 2>/dev/null; then
    IFACE=$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')
    if [ -n "$IFACE" ]; then
      OLD=$(sysctl -n "net.ipv6.conf.$IFACE.disable_ipv6" 2>/dev/null || true)
      if [ "$OLD" = "0" ]; then
        SUDO sysctl -w "net.ipv6.conf.$IFACE.disable_ipv6=1" >/dev/null
        PIP_IPV6_IFACE="$IFACE"
        PIP_IPV6_OLD="$OLD"
        trap 'if [ -n "$PIP_IPV6_IFACE" ]; then echo "$SUDOPW" | sudo -S -p "" sysctl -w "net.ipv6.conf.$PIP_IPV6_IFACE.disable_ipv6=$PIP_IPV6_OLD" >/dev/null 2>&1 || true; fi' EXIT
        echo "IPv6 unreachable: disabled temporarily on $IFACE for pip"
      fi
    fi
  fi
fi
"$VENV/bin/python" -m pip install -q --upgrade pip wheel "${PIP_ARGS[@]}"
EXTRAS=@@EXTRAS@@
if [ -n "$EXTRAS" ]; then
  SPEC="$SERVER[$EXTRAS]"
else
  SPEC="$SERVER"
fi
"$VENV/bin/python" -m pip install --progress-bar off "${PIP_ARGS[@]}" -e "$SPEC"
if [ -n "$PIP_IPV6_IFACE" ]; then
  echo "$SUDOPW" | sudo -S -p '' sysctl -w "net.ipv6.conf.$PIP_IPV6_IFACE.disable_ipv6=$PIP_IPV6_OLD" >/dev/null 2>&1 || true
  PIP_IPV6_IFACE=""
  trap - EXIT
fi

echo "== [5/7] persistent mount =="
if [ @@SKIP_FSTAB@@ != "1" ] && [ -d "$LIB" ]; then
  TARGET=$(findmnt -T "$LIB" -no TARGET 2>/dev/null || true)
  if [ -n "$TARGET" ] && [ "$TARGET" != "/" ]; then
    TARGET_ESC=${TARGET// /\\040}
    PRESENT=$(awk -v t="$TARGET" -v e="$TARGET_ESC" '$1 !~ /^#/ && ($2 == t || $2 == e) {print 1}' /etc/fstab 2>/dev/null | head -1 || true)
    if [ "$PRESENT" != "1" ]; then
      SRC=$(findmnt -T "$LIB" -no SOURCE)
      FSTYPE=$(findmnt -T "$LIB" -no FSTYPE)
      UUID=$(findmnt -T "$LIB" -no UUID 2>/dev/null || true)
      if [ -z "$UUID" ]; then
        UUID=$(lsblk -no UUID "$SRC" 2>/dev/null | head -1 || true)
      fi
      SPEC="$SRC"
      if [ -n "$UUID" ]; then SPEC="UUID=$UUID"; fi
      OPTS="defaults,nofail"
      case "$FSTYPE" in
        fuseblk|ntfs) FSTYPE=ntfs-3g; OPTS="$OPTS,uid=$(id -u),gid=$(id -g),umask=000" ;;
      esac
      if [ ! -f /etc/fstab.bak.localnetviewer ]; then
        SUDO cp /etc/fstab /etc/fstab.bak.localnetviewer
      fi
      printf '\n# LocalNetViewer book library\n%s %s %s %s 0 0\n' "$SPEC" "$TARGET_ESC" "$FSTYPE" "$OPTS" | SUDO tee -a /etc/fstab >/dev/null
      SUDO systemctl daemon-reload
      echo "fstab added: $SPEC $TARGET $FSTYPE $OPTS"
    else
      echo "fstab already has $TARGET"
    fi
  else
    echo "library is not on a separate mount, skip fstab"
  fi
fi

echo "== [6/7] systemd service =="
SUDO install -m 644 "$STAGING/$SERVICE.service" "/etc/systemd/system/$SERVICE.service"
SUDO systemctl daemon-reload
SUDO systemctl enable "$SERVICE" >/dev/null 2>&1 || true
SUDO systemctl restart "$SERVICE"
sleep 5
SUDO systemctl --no-pager --full status "$SERVICE" | head -12 || true

echo "== [7/7] local smoke test =="
OK=0
for _ in $(seq 1 15); do
  if curl -fsS --max-time 5 "http://127.0.0.1:@@PORT@@/api/v1/server/info" > "$STAGING/info.json"; then
    OK=1
    break
  fi
  sleep 2
done
if [ "$OK" != "1" ]; then
  SUDO journalctl -u "$SERVICE" --no-pager -n 40 || true
  echo "ERROR: service is not answering on port @@PORT@@" >&2
  exit 1
fi
cat "$STAGING/info.json"
echo
curl -fsS --max-time 30 "http://127.0.0.1:@@PORT@@/api/v1/files?path=/&limit=3"
echo
echo "remote deployment finished"
"""


def render_install_script(args, remote_dir: str, staging: str) -> str:
    extras = args.extras.strip()
    replacements = {
        "@@REMOTE_DIR@@": shlex.quote(remote_dir),
        "@@STAGING@@": shlex.quote(staging),
        "@@SERVICE@@": shlex.quote(args.service),
        "@@LIB@@": shlex.quote(args.library),
        "@@PORT@@": str(args.server_port),
        "@@SKIP_APT@@": "1" if args.skip_apt else "0",
        "@@SKIP_FSTAB@@": "1" if args.skip_fstab else "0",
        "@@PIP_TIMEOUT@@": str(args.pip_timeout),
        "@@PIP_IPV4_FIX@@": "1" if args.pip_ipv4_fallback else "0",
        "@@PIP_INDEX@@": shlex.quote(args.pip_index_url) if args.pip_index_url else "",
        "@@EXTRAS@@": shlex.quote(extras),
    }
    text = INSTALL_TEMPLATE
    for key, value in replacements.items():
        text = text.replace(key, value)
    return text


# ---------------------------------------------------------------------- 校验
def normalize_remote_dir(value: str, home: str) -> str:
    value = value.strip()
    if value == "~" or value.startswith("~/"):
        value = home + value[1:]
    if not value.startswith("/"):
        raise SystemExit(f"--remote-dir 必须是绝对路径或以 ~ 开头：{value!r}")
    if any(ch.isspace() for ch in value):
        raise SystemExit("--remote-dir 不能包含空白字符（systemd WorkingDirectory 限制）")
    return value.rstrip("/") or "/"


def validate_args(args) -> None:
    import re

    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.service):
        raise SystemExit("--service 只能包含字母数字和 _ . -")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.user):
        raise SystemExit("--user 只能包含字母数字和 _ . -")
    if args.group and not re.fullmatch(r"[A-Za-z0-9_.-]+", args.group):
        raise SystemExit("--group 只能包含字母数字和 _ . -")
    if not args.library.startswith("/"):
        raise SystemExit("--library 必须是绝对路径")
    if any(ch.isspace() for ch in args.remote_dir):
        raise SystemExit("--remote-dir 不能包含空白字符（systemd WorkingDirectory 限制）")
    if not re.fullmatch(r"[A-Za-z0-9_,-]*", args.extras):
        raise SystemExit("--extras 只能包含字母数字、逗号、下划线和连字符")
    for name in ("library", "root_name", "library_password", "auth_token", "bind", "font"):
        value = getattr(args, name)
        if value and ("\n" in value or "\r" in value):
            raise SystemExit(f"--{name.replace('_', '-')} 不能包含换行")


# ---------------------------------------------------------------------- 主流程
def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="远程部署并启动 LocalNetViewer server（SSH + systemd）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--host", default="192.168.0.102", help="SSH 主机地址")
    p.add_argument("--ssh-port", type=int, default=22, help="SSH 端口")
    p.add_argument("--user", default="orangepi", help="SSH 用户（同时作为服务运行用户）")
    p.add_argument("--group", default="", help="服务运行组，默认与 --user 相同")
    p.add_argument("--password", default="", help="SSH 密码；默认读环境变量 LNV_SSH_PASSWORD")
    p.add_argument("--sudo-password", default="", help="sudo 密码，默认与 SSH 密码相同")
    p.add_argument("--key", default="", help="SSH 私钥路径（优先使用；有密码时不用）")
    p.add_argument("--library", default="/mnt/usbdrive/book", help="远程书库目录")
    p.add_argument("--root-name", default="书库", help="客户端看到的书库名")
    p.add_argument("--library-password", default="", help="书库访问密码（留空 = 公开）")
    p.add_argument("--auth-token", default="", help="管理员令牌（留空 = 不启用）")
    p.add_argument("--bind", default="0.0.0.0", help="服务监听地址")
    p.add_argument("--server-port", type=int, default=8765, help="服务端口")
    p.add_argument("--remote-dir", default="~/localnetviewer", help="远程部署目录（代码/venv/cache）")
    p.add_argument("--service", default="localnetviewer", help="systemd 服务名")
    p.add_argument("--scan-mode", choices=["lazy", "eager"], default="lazy", help="书库扫描模式")
    p.add_argument("--font", default="", help="render.default_font，如 NotoSansCJK-Regular")
    p.add_argument("--extras", default="watch,sync", help="安装的 pip extras（留空不装）")
    p.add_argument("--pip-index-url", default=DEFAULT_PIP_INDEX, help="pip 镜像；置空使用默认 PyPI")
    p.add_argument("--pip-timeout", type=int, default=30, help="pip 网络超时（秒）")
    p.add_argument(
        "--pip-ipv4-fallback",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="pip 前检测 IPv6，不通时临时禁用默认网卡 IPv6 并在结束后恢复",
    )
    p.add_argument("--parsed-cache-mb", type=int, default=1024, help="解析缓存磁盘上限")
    p.add_argument("--page-cache-mb", type=int, default=256, help="页面内存缓存上限")
    p.add_argument("--page-cache-pages", type=int, default=256, help="页面内存缓存张数上限")
    p.add_argument("--layouts-cache-mb", type=int, default=512, help="布局缓存磁盘上限")
    p.add_argument("--layout-memory", type=int, default=16, help="内存保留的 PageMap 数")
    p.add_argument("--doc-memory", type=int, default=2, help="内存保留的解析结果数")
    p.add_argument("--skip-apt", action="store_true", help="不安装系统依赖（python3-venv/中文字体）")
    p.add_argument("--skip-fstab", action="store_true", help="不写入持久挂载 fstab（重启后书库可能不可见）")
    p.add_argument("--keep-staging", action="store_true", help="保留远程暂存目录（排错用）")
    p.add_argument("--no-local-check", action="store_true", help="部署后不从本机访问 HTTP 校验")
    p.add_argument("--timeout", type=int, default=3600, help="远程安装步骤超时（秒）")
    return p.parse_args(argv)


def local_http_check(host: str, port: int) -> None:
    host_part = f"[{host}]" if ":" in host and not host.startswith("[") else host
    url = f"http://{host_part}:{port}/api/v1/server/info"
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print(f"[warn] 本机访问 {url} 失败：{exc}（服务可能已启动，仅本机/防火墙路径不可达）")
        return
    libs = data.get("libraries") or []
    print(
        f"[ok] LAN check {url}: version={data.get('version')} "
        f"libraries={len(libs)} books={sum(int(x.get('book_count') or 0) for x in libs)}"
    )


def main(argv=None) -> int:
    args = parse_args(argv)
    validate_args(args)
    if not SERVER_DIR.is_dir():
        raise SystemExit(f"找不到 server 目录：{SERVER_DIR}")
    for name in ("pyproject.toml", "run.py"):
        if not (SERVER_DIR / name).is_file():
            raise SystemExit(f"server 目录缺少 {name}：{SERVER_DIR}")

    auth_password = args.password or os.environ.get("LNV_SSH_PASSWORD") or ""
    sudo_password = args.sudo_password or auth_password
    if not auth_password and not args.key:
        auth_password = getpass.getpass(f"{args.user}@{args.host} 的 SSH 密码: ")
        sudo_password = sudo_password or auth_password
    if not sudo_password:
        sudo_password = getpass.getpass("sudo 密码: ")

    client = connect(args, auth_password)
    remote = Remote(client, args.host)
    fd, archive_name = tempfile.mkstemp(prefix="lnv-server-", suffix=".tar.gz")
    os.close(fd)
    archive_path = Path(archive_name)
    staging = ""
    try:
        rc, home = remote.capture("printf %s \"$HOME\"", timeout=30)
        home = home.strip()
        if rc != 0 or not home.startswith("/"):
            raise SystemExit(f"无法获取远程 HOME：{home!r}")

        remote_dir = normalize_remote_dir(args.remote_dir, home)
        staging = f"{remote_dir}/.deploy"

        print(f"[deploy] {args.user}@{args.host} -> {remote_dir}")
        builder = f"set -e; mkdir -p {shlex.quote(staging)}"
        rc, out = remote.capture(builder, timeout=60)
        if rc != 0:
            raise RuntimeError(f"创建远程目录失败：{out}")

        print("[deploy] packing server/ ...")
        build_archive(archive_path)

        files = {
            "server.tar.gz": archive_path.read_bytes(),
            "config.yaml": render_config(args, remote_dir).encode("utf-8"),
            f"{args.service}.service": render_unit(args, remote_dir).encode("utf-8"),
            "install.sh": render_install_script(args, remote_dir, staging).encode("utf-8"),
        }
        sftp = client.open_sftp()
        try:
            for name, data in files.items():
                target = f"{staging}/{name}"
                with sftp.file(target, "wb") as fh:
                    fh.write(data)
                print(f"[deploy] uploaded {name} ({len(data)} bytes)")
        finally:
            sftp.close()

        print("[deploy] running remote install ...")
        rc = remote.stream(
            f"bash {shlex.quote(staging + '/install.sh')}",
            timeout=args.timeout,
            stdin_line=sudo_password,
        )
        if rc != 0:
            raise RuntimeError(
                f"远程安装失败（exit {rc}）；暂存目录已保留用于排错：{staging}"
            )

        if not args.keep_staging:
            remote.capture(f"rm -rf {shlex.quote(staging)}", timeout=60)
            staging = ""

        if not args.no_local_check:
            local_http_check(args.host, args.server_port)

        print()
        print("Deployed.")
        print(f"  URL:     http://{args.host}:{args.server_port}")
        print(f"  Docs:    http://{args.host}:{args.server_port}/docs")
        print(f"  Service: {args.service} (enabled, running)")
        print(f"  Source:  {remote_dir}/server")
        print(f"  Library: {args.library} ({args.root_name})")
        print(f"  Logs:    ssh {args.user}@{args.host} 'sudo journalctl -u {args.service} -f'")
        if args.keep_staging:
            print(f"  Staging: {staging}")
        return 0
    except (paramiko.SSHException, OSError, RuntimeError, TimeoutError) as exc:
        print(f"\n[error] {exc}", file=sys.stderr)
        if staging:
            print(f"[error] remote staging kept: {staging}", file=sys.stderr)
        return 1
    finally:
        try:
            archive_path.unlink(missing_ok=True)
        except OSError:
            pass
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
