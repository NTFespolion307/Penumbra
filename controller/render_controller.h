#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "controller/scheduler.h"
#include "controller/worker_link.h"
#include "core/film/film.h"

namespace pen {

struct RenderProgress {
  uint64_t samples_done = 0, samples_total = 0;  // pixel samples
  uint32_t items_done = 0, items_total = 0;
  double elapsed_seconds = 0;
};

struct DeviceTally {
  std::string name;
  uint32_t tiles = 0;
  uint64_t samples = 0;
  double seconds = 0;
};

struct RenderOutcome {
  bool complete = false;      // every item merged
  bool time_limited = false;  // stopped by --time-limit
  uint64_t nan_count = 0;
  double seconds = 0;
  std::vector<DeviceTally> devices;
};

// Runs one render: owns the scheduler and the film, hands items to workers through WorkerLink and merges
// their results (docs/GUIDE.md "Render lifecycle", steps 3-8).
class RenderController final : public WorkerEvents {
 public:
  RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene);

  // The link must be created with this controller as its WorkerEvents.
  void addWorker(std::unique_ptr<WorkerLink> link);
  WorkerId nextWorkerId() const { return static_cast<WorkerId>(workers_.size()); }

  // Blocks until the render completes (or hits the time limit); calls progress about once per second.
  RenderOutcome run(const std::function<void(const RenderProgress&)>& progress);
  const Film& film() const { return film_; }

  void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) override;
  void onRequestWork(WorkerId worker, DeviceId device, int count) override;
  void onTileResult(WorkerId worker, TileResult&& result) override;
  void onRelease(WorkerId worker, std::span<const LeaseId> leases) override;
  void onGone(WorkerId worker, GoneReason reason, const std::string& detail) override;

 private:
  RenderProgress progressLocked() const;

  RenderSettings settings_;
  std::shared_ptr<const Scene> scene_;
  Film film_;
  Scheduler scheduler_;
  std::vector<std::unique_ptr<WorkerLink>> workers_;
  std::map<std::pair<WorkerId, DeviceId>, DeviceTally> tallies_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_issuing_ = false;
  bool finished_ = false;
  std::string failure_;
  uint64_t samples_done_ = 0;
  uint64_t nan_count_ = 0;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace pen
