#include "lnv/netinfo.h"
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cctype>

#pragma comment(lib, "iphlpapi.lib")

namespace lnv {

namespace {
std::string wide_to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool is_virtual_adapter(const std::string& label_lower) {
    static const char* keys[] = {"vpn", "tap", "tun", "docker", "wsl", "virtual",
                                 "hamachi", "zerotier", "tailscale"};
    for (const char* k : keys)
        if (label_lower.find(k) != std::string::npos) return true;
    return false;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
}  // namespace

std::vector<NetAddr> enumerate_addresses() {
    // 仅枚举 IPv4（GetAdaptersAddresses(AF_INET, ...) + 下面的 AF_INET 过滤）；
    // Python 参照版会同时输出 IPv6（netinfo.py:89），本移植不实现 IPv6，
    // 因此 url_host 恒为点分四段、不需要方括号。
    std::vector<NetAddr> out;
    ULONG buf_len = 15 * 1024;
    std::vector<char> buf;
    for (int tries = 0; tries < 3; tries++) {
        buf.resize(buf_len);
        auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                       nullptr, addrs, &buf_len);
        if (r == ERROR_BUFFER_OVERFLOW) continue;
        if (r != NO_ERROR) return out;
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            std::string label = wide_to_utf8(a->FriendlyName);
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                sockaddr_storage ss{};
                memcpy(&ss, u->Address.lpSockaddr, u->Address.iSockaddrLength);
                if (ss.ss_family != AF_INET) continue;
                char ip[64] = {0};
                auto* sa = reinterpret_cast<sockaddr_in*>(&ss);
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                std::string s = ip;
                if (s.rfind("169.254.", 0) == 0) continue;  // 链路本地
                std::string lab = label;
                if (is_virtual_adapter(to_lower(lab))) lab += "，可能不可达";
                out.push_back({s, lab});
            }
        }
        break;
    }
    return out;
}

std::string firewall_hint(int port) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "netsh advfirewall firewall add rule name=\"LocalNetViewer\" "
                  "dir=in action=allow protocol=TCP localport=%d",
                  port);
    return buf;
}

}  // namespace lnv
