#include "apps/penumbra/ui/model.h"

#include <algorithm>
#include <fmt/format.h>

#include "backends/gpu/nvml.h"
#include "core/version.h"
#include "worker/device_manager.h"

namespace pen::ui {

namespace {

const History kEmpty;

std::string backendLabel(BackendKind b) {
  switch (b) {
    case BackendKind::Vulkan: return "Vulkan RT";
    case BackendKind::Cuda: return "CUDA";
    case BackendKind::Cpu: return "CPU";
    default: return std::string(toString(b));
  }
}

}  // namespace

Model::Model(HostingSession& session) : session_(session), system_(querySystem()) {
  cpu_.sample();  // the first sample only sets the baseline
}

void Model::sampleLocal(HostRow& row) {
  const SystemInfo now = querySystem();
  row.id = 0;
  row.local = true;
  row.name = system_.host;
  row.os = system_.os;
  row.address = "local";
  row.version = fmt::format("{} (git {})", appVersion(), gitHash());
  row.role = "Controller \xC2\xB7 this machine";
  row.cpu = static_cast<float>(cpu_.sample());
  row.ram_total = now.ram_total;
  row.ram_used = now.ram_total - now.ram_available;
  row.cores = system_.logical_cores;
  row.cpu_model = system_.cpu;
  row.since = {};
  // GPUs that can render (docs/GUIDE.md "--backend auto"): NVML readings where the driver has them.
  for (const RenderGpu& g : gpuInventory().gpus) {
    GpuRow r;
    r.name = g.name;
    r.backend = g.vulkan >= 0 ? "Vulkan RT" : "CUDA";
    r.vram_total = g.vram_bytes;
    if (const auto live = queryGpuLiveStats(g.uuid)) {
      r.usage = live->usage_percent;
      r.vram_used = live->vram_used;
      r.vram_total = live->vram_total;
      r.temperature_c = live->temperature_c;
      r.usage_from_driver = true;
    }
    row.gpus.push_back(r);
  }
}

bool Model::update() {
  const auto now = std::chrono::steady_clock::now();
  if (last_ != std::chrono::steady_clock::time_point{} && now - last_ < std::chrono::seconds(1)) return false;
  last_ = now;

  std::vector<HostRow> rows;
  HostRow local;
  sampleLocal(local);
  rows.push_back(std::move(local));

  double samples = 0;
  for (const HostView& h : session_.hosts()) {
    HostRow r;
    r.id = h.id;
    r.name = h.info.name;
    r.os = h.info.os;
    r.address = h.info.address;
    r.version = h.info.version;
    r.role = "Worker";
    r.connected = h.connected;
    r.in_render = h.in_render;
    r.cores = h.info.logical_cores;
    r.ram_total = h.info.ram_total;
    r.since = h.since;
    r.log = h.log;
    for (const DeviceInfo& d : h.info.devices) {
      GpuRow g;
      g.name = d.name;
      g.backend = backendLabel(d.backend);
      g.vram_total = d.backend == BackendKind::Cpu ? 0 : d.memory_bytes;
      r.gpus.push_back(g);
    }
    if (h.stats) {
      const WorkerStats& s = *h.stats;
      r.cpu = s.cpu_percent;
      r.ram_used = s.ram_used;
      r.ram_total = s.ram_total;
      r.rtt_ms = s.rtt_ms;
      r.upload_bytes_per_s = s.upload_bytes_per_s;
      r.upload_bound = s.upload_bound;
      r.cache_used = s.cache_used;
      r.cache_limit = s.cache_limit;
      r.cache_scenes = s.cache_scenes;
      for (const WorkerStats::Device& d : s.devices) {
        if (d.device >= r.gpus.size()) continue;
        GpuRow& g = r.gpus[d.device];
        g.usage = d.busy_percent;
        if (d.vram_total) {
          g.vram_used = d.vram_used;
          g.vram_total = d.vram_total;
        }
        g.temperature_c = d.temperature_c;
        g.tiles_per_s = d.tiles_per_s;
        g.usage_from_driver = d.temperature_c > -273;  // NVML gives both
        r.tiles_per_s += d.tiles_per_s;
        r.samples_per_s += d.samples_per_s;
      }
    }
    if (r.connected) samples += r.samples_per_s;
    rows.push_back(std::move(r));
  }
  hosts_ = std::move(rows);
  events_ = session_.events();

  for (const HostRow& h : hosts_) {
    if (!h.connected) continue;
    cpu_history_[h.id].push(std::max(0.0f, h.cpu));
    ram_history_[h.id].push(h.ram_total ? float(double(h.ram_used) / double(h.ram_total)) : 0.0f);
  }
  throughput_.push(static_cast<float>(samples));
  return true;
}

const HostRow* Model::host(WorkerId id) const {
  for (const HostRow& h : hosts_)
    if (h.id == id) return &h;
  return nullptr;
}

uint32_t Model::hostsOnline() const {
  uint32_t n = 0;
  for (const HostRow& h : hosts_) n += h.connected;
  return n;
}

uint32_t Model::gpuCount() const {
  uint32_t n = 0;
  for (const HostRow& h : hosts_)
    if (h.connected)
      for (const GpuRow& g : h.gpus) n += g.backend != "CPU";
  return n;
}

uint32_t Model::gpusRendering() const {
  uint32_t n = 0;
  for (const HostRow& h : hosts_)
    if (h.connected && h.in_render)
      for (const GpuRow& g : h.gpus) n += g.backend != "CPU";
  return n;
}

float Model::clusterCpu() const {
  double weighted = 0, cores = 0;
  for (const HostRow& h : hosts_) {
    if (!h.connected || h.cpu < 0 || !h.cores) continue;
    weighted += double(h.cpu) * h.cores;
    cores += h.cores;
  }
  return cores > 0 ? float(weighted / cores) : 0.0f;
}

uint32_t Model::clusterThreads() const {
  uint32_t n = 0;
  for (const HostRow& h : hosts_) n += h.connected ? h.cores : 0;
  return n;
}

uint64_t Model::clusterRamUsed() const {
  uint64_t n = 0;
  for (const HostRow& h : hosts_) n += h.connected ? h.ram_used : 0;
  return n;
}

uint64_t Model::clusterRamTotal() const {
  uint64_t n = 0;
  for (const HostRow& h : hosts_) n += h.connected ? h.ram_total : 0;
  return n;
}

const History& Model::cpuHistory(WorkerId id) const {
  auto it = cpu_history_.find(id);
  return it == cpu_history_.end() ? kEmpty : it->second;
}

const History& Model::ramHistory(WorkerId id) const {
  auto it = ram_history_.find(id);
  return it == ram_history_.end() ? kEmpty : it->second;
}

}  // namespace pen::ui
