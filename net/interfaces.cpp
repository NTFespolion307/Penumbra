#include "net/interfaces.h"

#include <asio.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace pen::net {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool vpnInterfaceName(const std::string& name) {
  const std::string n = lower(name);
  return n.find("tailscale") != std::string::npos || n.find("wireguard") != std::string::npos ||
         n.rfind("wg", 0) == 0 || n.rfind("tun", 0) == 0 || n.find("zerotier") != std::string::npos;
}

void add(std::vector<HostAddress>& out, const asio::ip::address& a, const std::string& iface) {
  if (a.is_loopback()) return;
  HostAddress h;
  h.address = a.to_string();
  h.interface = iface;
  if (a.is_v4()) {
    const auto b = a.to_v4().to_bytes();
    if (b[0] == 169 && b[1] == 254) return;  // link-local
    if (isSharedAddressSpace(h.address) || vpnInterfaceName(iface)) h.kind = HostAddress::Kind::Vpn;
    else if (isPrivateIpv4(h.address)) h.kind = HostAddress::Kind::Lan;
    else h.kind = HostAddress::Kind::Other;
  } else {
    const auto v6 = a.to_v6();
    if (v6.is_link_local() || v6.is_v4_mapped()) return;
    const auto b = v6.to_bytes();
    const bool tailscale = b[0] == 0xfd && b[1] == 0x7a && b[2] == 0x11 && b[3] == 0x5c && b[4] == 0xa1 && b[5] == 0xe0;
    if (!tailscale && !vpnInterfaceName(iface)) return;  // IPv6 LAN addresses would only repeat the IPv4 ones
    h.kind = HostAddress::Kind::Vpn;
  }
  out.push_back(std::move(h));
}

}  // namespace

const char* toString(HostAddress::Kind kind) {
  switch (kind) {
    case HostAddress::Kind::Lan: return "LAN";
    case HostAddress::Kind::Vpn: return "VPN";
    case HostAddress::Kind::Other: return "public or other";
  }
  return "?";
}

bool isSharedAddressSpace(const std::string& ipv4) {
  std::error_code ec;
  const auto a = asio::ip::make_address_v4(ipv4, ec);
  if (ec) return false;
  const auto b = a.to_bytes();
  return b[0] == 100 && (b[1] & 0xC0) == 64;
}

bool isPrivateIpv4(const std::string& ipv4) {
  std::error_code ec;
  const auto a = asio::ip::make_address_v4(ipv4, ec);
  if (ec) return false;
  const auto b = a.to_bytes();
  return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168);
}

std::vector<HostAddress> listHostAddresses() {
  std::vector<HostAddress> out;
#if defined(_WIN32)
  ULONG size = 16384;
  std::vector<unsigned char> buf(size);
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    std::string name;
    if (a->FriendlyName) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
      name.resize(n > 0 ? size_t(n - 1) : 0);
      if (n > 1) WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name.data(), n, nullptr, nullptr);
    }
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const sockaddr* sa = u->Address.lpSockaddr;
      if (sa->sa_family == AF_INET) {
        asio::ip::address_v4::bytes_type b;
        std::memcpy(b.data(), &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
        add(out, asio::ip::address_v4(b), name);
      } else if (sa->sa_family == AF_INET6) {
        asio::ip::address_v6::bytes_type b;
        std::memcpy(b.data(), &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, 16);
        add(out, asio::ip::address_v6(b), name);
      }
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || !(i->ifa_flags & IFF_UP)) continue;
    const std::string name = i->ifa_name ? i->ifa_name : "";
    if (i->ifa_addr->sa_family == AF_INET) {
      asio::ip::address_v4::bytes_type b;
      std::memcpy(b.data(), &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, 4);
      add(out, asio::ip::address_v4(b), name);
    } else if (i->ifa_addr->sa_family == AF_INET6) {
      asio::ip::address_v6::bytes_type b;
      std::memcpy(b.data(), &reinterpret_cast<const sockaddr_in6*>(i->ifa_addr)->sin6_addr, 16);
      add(out, asio::ip::address_v6(b), name);
    }
  }
  freeifaddrs(list);
#endif
  // LAN first, then VPN, then the rest.
  std::stable_sort(out.begin(), out.end(), [](const HostAddress& a, const HostAddress& b) { return a.kind < b.kind; });
  return out;
}

}  // namespace pen::net
