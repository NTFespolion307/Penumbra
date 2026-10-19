#pragma once

#include <string>
#include <vector>

namespace pen::net {

// An address workers could use to reach this machine (docs/GUIDE.md "Addresses, NAT and firewall").
struct HostAddress {
  enum class Kind { Lan, Vpn, Other };
  std::string address;    // "192.168.1.20", "100.101.102.103", "fd7a:115c:a1e0::1"
  std::string interface;  // adapter name as the OS shows it
  Kind kind = Kind::Other;
};

// Every IPv4 address and every VPN IPv6 address of this machine's interfaces that are up, without loopback and
// link-local ones: LAN (private ranges), VPN (Tailscale 100.64.0.0/10 or fd7a:115c:a1e0::/48, WireGuard and
// Tailscale adapters), other (public addresses).
std::vector<HostAddress> listHostAddresses();

const char* toString(HostAddress::Kind kind);

// True for 100.64.0.0/10: carrier-grade NAT on a router's WAN side, Tailscale on a local interface.
bool isSharedAddressSpace(const std::string& ipv4);
// True for 10/8, 172.16/12, 192.168/16.
bool isPrivateIpv4(const std::string& ipv4);

}  // namespace pen::net
