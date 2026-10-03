"""本机可用地址枚举（设计文档 4.10）。

多网卡 / VPN 环境下必须把所有候选地址都打印出来并标注网卡类型，
避免客户端连到 VPN 地址（10.8.x.x 之类）后发现不可达。

优先使用 psutil 拿到网卡名与状态；未安装时退化为纯标准库枚举。
"""

from __future__ import annotations

import ipaddress
import socket
from dataclasses import dataclass
from typing import List

# 网卡名关键字 → 提示。命中即认为"可能不可达"，排在后面。
_VIRTUAL_HINTS = (
    "vpn",
    "tap",
    "tun",
    "wintun",
    "wireguard",
    "openvpn",
    "zerotier",
    "tailscale",
    "vmware",
    "virtualbox",
    "vbox",
    "hyper-v",
    "vethernet",
    "docker",
    "wsl",
    "loopback",
    "bluetooth",
    "radmin",
    "hamachi",
)


@dataclass(frozen=True)
class AddressInfo:
    ip: str
    adapter: str
    label: str
    recommended: bool

    @property
    def url_host(self) -> str:
        return f"[{self.ip}]" if ":" in self.ip else self.ip


def _classify(ip: str, adapter: str) -> tuple[str, bool]:
    """返回 (类型说明, 是否推荐)。"""
    name = (adapter or "").lower()
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return "未知", False

    if addr.is_loopback:
        return "本机回环", False
    if addr.is_link_local:
        return "链路本地（不可用）", False
    if any(h in name for h in _VIRTUAL_HINTS):
        return f"虚拟网卡（{adapter}，客户端可能不可达）", False

    if addr.version == 6:
        return f"IPv6（{adapter or '未知网卡'}）", False
    if addr.is_private:
        if name.startswith(("eth", "en", "wl", "wlan", "wi-fi", "ethernet", "本地连接", "以太网", "无线")):
            return f"{adapter}（推荐）", True
        return f"{adapter or '局域网'}", True
    return f"公网/其它（{adapter or '未知网卡'}）", False


def _from_psutil() -> List[tuple[str, str]]:
    try:
        import psutil  # type: ignore
    except Exception:
        return []
    out: List[tuple[str, str]] = []
    try:
        stats = psutil.net_if_stats()
        for name, addrs in psutil.net_if_addrs().items():
            st = stats.get(name)
            if st is not None and not st.isup:
                continue
            for a in addrs:
                if a.family in (socket.AF_INET, socket.AF_INET6):
                    ip = a.address.split("%")[0]
                    out.append((ip, name))
    except Exception:
        return []
    return out


def _from_stdlib() -> List[tuple[str, str]]:
    """无 psutil 时的退化方案：hostname 解析 + 主路由探测。"""
    out: List[tuple[str, str]] = []
    seen: set[str] = set()

    def _add(ip: str, name: str) -> None:
        ip = ip.split("%")[0]
        if ip and ip not in seen:
            seen.add(ip)
            out.append((ip, name))

    try:
        infos = socket.getaddrinfo(socket.gethostname(), None)
        for info in infos:
            _add(info[4][0], "")
    except OSError:
        pass

    # 探测默认出口网卡地址（不会真的发包）
    for probe in (("8.8.8.8", 80), ("1.1.1.1", 80)):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(probe)
            _add(s.getsockname()[0], "默认出口")
        except OSError:
            pass
        finally:
            s.close()

    _add("127.0.0.1", "loopback")
    return out


def enumerate_addresses(exclude_loopback: bool = True) -> List[AddressInfo]:
    """枚举本机 IPv4/IPv6 地址，推荐的在前。"""
    raw = _from_psutil() or _from_stdlib()
    result: List[AddressInfo] = []
    seen: set[str] = set()
    for ip, adapter in raw:
        if ip in seen:
            continue
        seen.add(ip)
        label, recommended = _classify(ip, adapter)
        if exclude_loopback and label == "本机回环":
            continue
        result.append(AddressInfo(ip=ip, adapter=adapter, label=label, recommended=recommended))
    result.sort(key=lambda a: (not a.recommended, a.ip))
    return result


def firewall_hint(port: int) -> str:
    """Windows 首次运行需要放行端口的命令（设计 4.10）。"""
    return (
        f'netsh advfirewall firewall add rule name="LocalNetViewer {port}" '
        f"dir=in action=allow protocol=TCP localport={port}"
    )
