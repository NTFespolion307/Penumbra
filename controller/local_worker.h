#pragma once

#include <memory>
#include <vector>

#include "controller/worker_link.h"
#include "worker/device_slot.h"

namespace pen {

// The controller's own devices, connected to the scheduler in-process through the same WorkerLink interface
// remote workers use (docs/GUIDE.md "Local worker").
class LocalWorker final : public WorkerLink {
 public:
  LocalWorker(WorkerId id, WorkerEvents& events, std::vector<std::unique_ptr<IBackend>> devices);
  ~LocalWorker() override;

  WorkerInfo info() const override;
  void offerScene(std::shared_ptr<const Scene> scene, const RenderSettings& settings) override;
  void sendLease(const Lease& lease) override;
  void cancel(std::span<const LeaseId> leases) override;
  void ackTile(LeaseId lease, AckStatus status) override;
  void pause(bool paused) override;
  void complete() override;

 private:
  WorkerId id_;
  WorkerEvents& events_;
  std::vector<std::unique_ptr<IBackend>> devices_;
  std::vector<std::unique_ptr<DeviceSlot>> slots_;
  std::shared_ptr<const Scene> scene_;
};

}  // namespace pen
