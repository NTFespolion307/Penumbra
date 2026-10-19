#pragma once

#include <atomic>
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
  uint64_t samples_done = 0, samples_total = 0;  // pixel samples, including tiles restored on resume
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
  bool interrupted = false;   // stopped by Ctrl+C
  bool out_of_disk = false;   // a film write failed for lack of space
  std::string error;          // film write error text, if any
  uint64_t nan_count = 0;
  double seconds = 0;
  std::vector<DeviceTally> devices;
};

struct RenderRunOptions {
  const std::atomic<bool>* interrupt = nullptr;                 // set by the Ctrl+C handler
  std::function<void(const RenderProgress&)> progress;          // about once per second
  std::function<void(const RenderProgress&)> after_checkpoint;  // after each periodic film checkpoint
};

// Runs one render: owns the scheduler, merges results into the film, hands items to workers through
// WorkerLink, checkpoints the film every --checkpoint (docs/GUIDE.md "Render lifecycle", steps 3-8).
class RenderController final : public WorkerEvents {
 public:
  RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene, Film& film);

  // The link must be created with this controller as its WorkerEvents.
  void addWorker(std::unique_ptr<WorkerLink> link);
  WorkerId nextWorkerId() const { return static_cast<WorkerId>(workers_.size()); }

  // Resume: skips items whose film tile is already rendered. Returns how many items were skipped.
  uint32_t skipRenderedTiles();

  RenderOutcome run(const RenderRunOptions& options);
  RenderProgress progress() const;

  void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) override;
  void onRequestWork(WorkerId worker, DeviceId device, int count) override;
  void onTileResult(WorkerId worker, TileResult&& result) override;
  void onRelease(WorkerId worker, std::span<const LeaseId> leases) override;
  void onGone(WorkerId worker, GoneReason reason, const std::string& detail) override;

 private:
  RenderProgress progressLocked() const;
  void stopLocked();  // no new work, cancel what is in flight, finish

  RenderSettings settings_;
  std::shared_ptr<const Scene> scene_;
  Film& film_;
  Scheduler scheduler_;
  std::vector<std::unique_ptr<WorkerLink>> workers_;
  std::map<std::pair<WorkerId, DeviceId>, DeviceTally> tallies_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_issuing_ = false;
  bool finished_ = false;
  bool out_of_disk_ = false;
  std::string failure_;     // device failure (fatal in M2: one local worker)
  std::string film_error_;  // film write failure
  uint64_t samples_done_ = 0;
  uint64_t nan_count_ = 0;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace pen
