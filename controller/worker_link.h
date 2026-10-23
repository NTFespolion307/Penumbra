#pragma once

// How the controller sees any worker, local (in-process) or remote (network, M4), and how workers report
// back (docs/GUIDE.md "Repository layout and core interfaces"). Every render goes through these, including
// solo renders.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "backends/backend.h"
#include "core/package/package.h"
#include "core/render/work.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

using WorkerId = uint32_t;
using DeviceId = uint32_t;

struct Lease {
  LeaseId id = 0;
  DeviceId device = 0;
  WorkItem item;
};

struct WorkerInfo {
  std::string name;
  bool local = false;
  std::vector<DeviceInfo> devices;
  // From the worker's Hello (network workers): for the Hosts page (docs/GUIDE.md "UI").
  std::string os;
  std::string address;  // where it connected from
  uint32_t logical_cores = 0;
  uint64_t ram_total = 0;
  std::string version;  // "0.1.0 (git 1a2b3c4d5e6f)"
};

// What a worker host reports every 2 s, also when idle (docs/GUIDE.md "Stats").
struct WorkerStats {
  struct Device {
    DeviceId device = 0;
    float busy_percent = 0;  // NVML usage on NVIDIA, else the share of time spent rendering
    uint64_t vram_used = 0, vram_total = 0;
    float temperature_c = -300;  // < -273 when unknown
    float tiles_per_s = 0;
    double samples_per_s = 0;
  };
  float cpu_percent = 0;
  uint64_t ram_used = 0, ram_total = 0;
  bool upload_bound = false;
  std::vector<Device> devices;
  float rtt_ms = 0;  // 0 = not measured yet
  double upload_bytes_per_s = 0;
  uint64_t cache_used = 0, cache_limit = 0;
  uint32_t cache_scenes = 0;
};

struct DeviceReady {
  DeviceId device = 0;
  BackendKind backend = BackendKind::Cpu;
  uint32_t memory_tier = 0;
  double build_seconds = 0;
  std::string summary;
  bool skipped = false;  // cannot take the scene (reason); it never asks for work (DECISIONS D-092)
  std::string reason;
};

// What every worker gets when a render starts: the loaded scene (in-process workers use it directly) and its
// package (network workers download it, or find it in their cache), plus the render settings.
struct RenderOffer {
  std::shared_ptr<const Scene> scene;
  std::shared_ptr<const ScenePackage> package;
  RenderSettings settings;
};

enum class AckStatus { Accepted, Duplicate, Rejected };
enum class GoneReason { Completed, Failed, Lost, Kicked };

class WorkerEvents {
 public:
  virtual ~WorkerEvents() = default;
  virtual void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) = 0;
  virtual void onRequestWork(WorkerId worker, DeviceId device, int count) = 0;
  virtual void onTileResult(WorkerId worker, TileResult&& result) = 0;
  // Leases handed back unfinished (device failed, worker leaving); their items are requeued.
  virtual void onRelease(WorkerId worker, std::span<const LeaseId> leases) = 0;
  // One device stopped for good; the worker continues with its other devices (docs/GUIDE.md "Device failure").
  virtual void onDeviceFailed(WorkerId worker, DeviceId device, const std::string& error) = 0;
  virtual void onGone(WorkerId worker, GoneReason reason, const std::string& detail) = 0;
  // A host's statistics (network workers, every 2 s); renders do not need them, the session and UI do.
  virtual void onStats(WorkerId worker, const WorkerStats& stats) {
    (void)worker;
    (void)stats;
  }
  // A warning or error the worker forwarded (docs/GUIDE.md "Logging"); also in the controller's log.
  enum class LogLevel { Info, Warning, Error };
  virtual void onLog(WorkerId worker, LogLevel level, const std::string& text) {
    (void)worker;
    (void)level;
    (void)text;
  }
};

class WorkerLink {
 public:
  virtual ~WorkerLink() = default;
  virtual WorkerInfo info() const = 0;
  virtual void offerScene(const RenderOffer& offer) = 0;
  virtual void sendLease(const Lease& lease) = 0;
  virtual void cancel(std::span<const LeaseId> leases) = 0;
  virtual void ackTile(LeaseId lease, AckStatus status) = 0;
  virtual void pause(bool paused) = 0;
  virtual void complete() = 0;
  // Disconnects a misbehaving worker with a reason (network workers; the local worker never is).
  virtual void kick(const std::string& reason) { (void)reason; }
};

}  // namespace pen
