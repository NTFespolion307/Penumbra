#pragma once

#include <cstdint>
#include <string>

namespace pen {

struct SystemInfo {
  std::string os;    // "Windows 11 (24H2, build 26200)", "Ubuntu 22.04.5 LTS (kernel 6.6.87)"
  std::string host;  // host name
  std::string cpu;   // CPU model name
  uint32_t logical_cores = 0;
  uint64_t ram_total = 0;      // bytes
  uint64_t ram_available = 0;  // bytes
};

SystemInfo querySystem();
std::string hostName();

// Whole-machine CPU use between two calls of sample(), in percent (0 on the first call).
class CpuUsage {
 public:
  double sample();

 private:
  uint64_t idle_ = 0, total_ = 0;
};

}  // namespace pen
