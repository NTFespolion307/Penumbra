#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pen {

// Asks the router to forward the hosting port to this machine (docs/GUIDE.md "Addresses, NAT and firewall"),
// on a background thread so hosting starts at once, and removes the mapping when destroyed. Every outcome is
// logged: mapped (with the public address), no UPnP router, refused, or a router whose own WAN address is not
// public (carrier-grade NAT: forwarding cannot work, use a VPN).
class UpnpMapping {
 public:
  explicit UpnpMapping(uint16_t port);
  ~UpnpMapping();
  UpnpMapping(const UpnpMapping&) = delete;
  UpnpMapping& operator=(const UpnpMapping&) = delete;
  // One line for the UI: "asking the router", "forwarded: 203.0.113.5:7420", "no UPnP router", ...
  std::string status() const;

 private:
  void run();

  uint16_t port_;
  std::thread thread_;
  mutable std::mutex mutex_;
  bool mapped_ = false;
  std::string status_ = "asking the router";
  void setStatus(std::string s);
  std::string control_url_, service_type_;
};

}  // namespace pen
