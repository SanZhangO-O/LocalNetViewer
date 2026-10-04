#pragma once
// 网卡地址枚举与防火墙提示（对应 Python app/netinfo.py）
#include <string>
#include <vector>

namespace lnv {

struct NetAddr {
    std::string url_host;  // 本移植仅枚举 AF_INET，恒为点分四段（不产生 IPv6 方括号形式）
    std::string label;
};

// 局域网可用地址（跳过回环/链路本地/Down）；虚拟网卡标注
std::vector<NetAddr> enumerate_addresses();

// Windows 防火墙放行命令
std::string firewall_hint(int port);

}  // namespace lnv
