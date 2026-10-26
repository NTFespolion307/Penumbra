#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "controller/hosting/hosting_session.h"
#include "core/system/sysinfo.h"

namespace pen::ui {

// One GPU (or the CPU device) of a host, as the Hosts pages show it.
struct GpuRow {
  std::string name;
  std::string backend;          // "Vulkan RT", "CUDA", "CPU"
  float usage = -1;             // %, < 0 unknown
  uint64_t vram_used = 0, vram_total = 0;
  float temperature_c = -300;   // < -273 unknown
  float tiles_per_s = 0;
  bool usage_from_driver = false;  // NVML; else Penumbra's busy %
};

// One host: this machine (id 0) or a worker that joined.
struct HostRow {
  WorkerId id = 0;
  bool local = false;
  std::string name, os, address, version, role;
  bool connected = true;
  bool in_render = false;
  float cpu = -1;  // %, < 0 unknown (no Stats yet)
  uint64_t ram_used = 0, ram_total = 0;
  uint32_t cores = 0;
  std::string cpu_model;
  std::vector<GpuRow> gpus;
  float tiles_per_s = 0;
  double samples_per_s = 0;
  float rtt_ms = 0;
  double upload_bytes_per_s = 0;
  uint64_t cache_used = 0, cache_limit = 0;
  uint32_t cache_scenes = 0;
  bool upload_bound = false;
  std::chrono::system_clock::time_point since;
  std::vector<SessionEvent> log;
};

// Last 10 minutes at one sample per second (docs/GUIDE.md "UI": charts keep the last 10 minutes).
class History {
 public:
  static constexpr size_t kSamples = 600;
  void push(float v) {
    values_.push_back(v);
    if (values_.size() > kSamples) values_.pop_front();
  }
  const std::deque<float>& values() const { return values_; }

 private:
  std::deque<float> values_;
};

// What the UI shows, sampled once per second from the hosting session and this machine (docs/GUIDE.md "Where the
// numbers come from"). Only the UI thread uses it.
class Model {
 public:
  explicit Model(HostingSession& session);

  // Samples everything if a second has passed since the last time; returns whether it did.
  bool update();

  const std::vector<HostRow>& hosts() const { return hosts_; }  // this machine first
  const HostRow* host(WorkerId id) const;
  const std::vector<SessionEvent>& events() const { return events_; }

  // Cluster totals (Dashboard).
  uint32_t hostsOnline() const;
  uint32_t gpuCount() const;
  uint32_t gpusRendering() const;
  float clusterCpu() const;  // weighted by cores
  uint32_t clusterThreads() const;
  uint64_t clusterRamUsed() const;
  uint64_t clusterRamTotal() const;
  double samplesPerSecond() const { return throughput_.values().empty() ? 0 : throughput_.values().back(); }
  const History& throughput() const { return throughput_; }  // samples/s
  const History& cpuHistory(WorkerId id) const;
  const History& ramHistory(WorkerId id) const;  // used / total, 0-1

  HostingSession& session() { return session_; }

 private:
  void sampleLocal(HostRow& row);

  HostingSession& session_;
  std::chrono::steady_clock::time_point last_{};
  std::vector<HostRow> hosts_;
  std::vector<SessionEvent> events_;
  History throughput_;
  std::map<WorkerId, History> cpu_history_, ram_history_;
  CpuUsage cpu_;
  SystemInfo system_;
};

}  // namespace pen::ui
