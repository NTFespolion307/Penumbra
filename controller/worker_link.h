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
};

struct DeviceReady {
  DeviceId device = 0;
  BackendKind backend = BackendKind::Cpu;
  uint32_t memory_tier = 0;
  double build_seconds = 0;
  std::string summary;
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
