#include "controller/hosting/upnp.h"

#include <fmt/format.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include <spdlog/spdlog.h>

#include <string>

#include "net/interfaces.h"

namespace pen {

namespace {
constexpr int kDiscoverMs = 2000;
const char kDescription[] = "Penumbra controller";
}  // namespace

UpnpMapping::UpnpMapping(uint16_t port) : port_(port) { thread_ = std::thread([this] { run(); }); }

void UpnpMapping::setStatus(std::string s) {
  std::lock_guard lock(mutex_);
  status_ = std::move(s);
}

std::string UpnpMapping::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void UpnpMapping::run() {
  int error = 0;
  UPNPDev* devices = upnpDiscover(kDiscoverMs, nullptr, nullptr, 0, 0, 2, &error);
  if (!devices) {
    spdlog::info("UPnP: no router answered, so TCP {} is not forwarded automatically. For internet workers, "
                 "forward it on the router by hand, or join over a VPN (see README)",
                 port_);
    setStatus("no UPnP router answered: forward TCP " + std::to_string(port_) + " by hand, or use a VPN");
    return;
  }
  UPNPUrls urls{};
  IGDdatas data{};
  char lan[64] = {}, wan[64] = {};
  const int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof(lan), wan, sizeof(wan));
  freeUPNPDevlist(devices);
  if (igd == 0) {
    spdlog::info("UPnP: no internet gateway found on the network; TCP {} is not forwarded automatically", port_);
    setStatus("no internet gateway found: forward TCP " + std::to_string(port_) + " by hand, or use a VPN");
    return;
  }
  const std::string wanIp = wan;
  if (igd == 2 || (!wanIp.empty() && (net::isPrivateIpv4(wanIp) || net::isSharedAddressSpace(wanIp)))) {
    // The router's own WAN side is not public: carrier-grade NAT or a second router in front.
    spdlog::warn("UPnP: the router's internet address ({}) is not public (carrier-grade NAT or a second router), "
                 "so port forwarding cannot reach this machine from the internet. Use a VPN such as Tailscale for "
                 "internet workers",
                 wanIp.empty() ? "unknown" : wanIp);
    setStatus("carrier-grade NAT: forwarding cannot work, use a VPN such as Tailscale");
    FreeUPNPUrls(&urls);
    return;
  }
  const std::string portText = std::to_string(port_);
  const int rc = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portText.c_str(), portText.c_str(), lan,
                                     kDescription, "TCP", nullptr, "0");
  if (rc != UPNPCOMMAND_SUCCESS) {
    spdlog::warn("UPnP: the router refused to forward TCP {} ({}); forward it by hand for internet workers", port_,
                 strupnperror(rc));
    setStatus(std::string("the router refused (") + strupnperror(rc) + "): forward TCP " + portText + " by hand");
    FreeUPNPUrls(&urls);
    return;
  }
  char external[64] = {};
  UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external);
  spdlog::info("UPnP: the router forwards TCP {} to this machine ({}); internet workers join with --join {}:{}", port_,
               lan, external[0] ? external : "<your public IP>", port_);
  setStatus(fmt::format("forwarded: {}:{}", external[0] ? external : "your public IP", port_));
  std::lock_guard lock(mutex_);
  mapped_ = true;
  control_url_ = urls.controlURL;
  service_type_ = data.first.servicetype;
  FreeUPNPUrls(&urls);
}

UpnpMapping::~UpnpMapping() {
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  if (!mapped_) return;
  const std::string portText = std::to_string(port_);
  const int rc = UPNP_DeletePortMapping(control_url_.c_str(), service_type_.c_str(), portText.c_str(), "TCP", nullptr);
  if (rc == UPNPCOMMAND_SUCCESS) spdlog::info("UPnP: removed the forwarding of TCP {}", port_);
  else spdlog::warn("UPnP: could not remove the forwarding of TCP {} ({})", port_, strupnperror(rc));
}

}  // namespace pen
