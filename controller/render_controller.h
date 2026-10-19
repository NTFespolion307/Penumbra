#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "controller/scheduler.h"
#include "controller/worker_link.h"
#include "core/film/film.h"

namespace pen {

struct RenderProgress {
  uint64_t samples_done = 0, samples_total = 0;  // pixel samples (total 0 = until the time limit)
  uint32_t items_done = 0, items_total = 0;      // tiles finished / tiles
  double elapsed_seconds = 0;
  double time_limit_seconds = 0;
};

struct DeviceTally {
  std::string name;
  uint32_t items = 0;
  uint64_t samples = 0;
  double seconds = 0;
  bool failed = false;
};

struct RenderOutcome {
  bool complete = false;      // every chunk of every tile merged
  bool time_limited = false;  // stopped by --time-limit
  bool interrupted = false;   // stopped by Ctrl+C
  bool out_of_disk = false;   // a film write failed for lack of space
  std::string error;          // film write error text, if any
  uint64_t nan_count = 0;
  uint32_t reissued = 0;      // leases handed out again (late or lost)
  double seconds = 0;
  std::vector<DeviceTally> devices;
};

struct RenderRunOptions {
  const std::atomic<bool>* interrupt = nullptr;                 // set by the Ctrl+C handler
  std::function<void(const RenderProgress&)> progress;          // about once per second
  std::function<void(const RenderProgress&)> after_checkpoint;  // after each periodic film checkpoint
};

// Runs one render (docs/GUIDE.md "Render lifecycle", steps 3-8; "Scheduling, leases and tiles"): hands work
// items to device slots as leases, merges each tile's chunks into the film strictly in chunk order (so any
// distribution of work gives the same bits, D-045), re-issues leases that take more than 10x the median item
// time, requeues the leases of failed devices, and checkpoints the film every --checkpoint.
class RenderController final : public WorkerEvents {
 public:
  RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene, Film& film, PassOrder order);

  // The link must be created with this controller as its WorkerEvents.
  void addWorker(std::unique_ptr<WorkerLink> link);
  WorkerId nextWorkerId() const { return static_cast<WorkerId>(workers_.size()); }

  // Resume: continue every tile from the samples already in the film. Returns tiles with data.
  uint32_t restoreFromFilm();

  RenderOutcome run(const RenderRunOptions& options);
  RenderProgress progress() const;

  void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) override;
  void onRequestWork(WorkerId worker, DeviceId device, int count) override;
  void onTileResult(WorkerId worker, TileResult&& result) override;
  void onRelease(WorkerId worker, std::span<const LeaseId> leases) override;
  void onDeviceFailed(WorkerId worker, DeviceId device, const std::string& error) override;
  void onGone(WorkerId worker, GoneReason reason, const std::string& detail) override;

 private:
  struct LeaseInfo {
    WorkItem item;
    WorkerId worker = 0;
    DeviceId device = 0;
    std::chrono::steady_clock::time_point issued;
    bool reissued = false;
  };
  using DeviceKey = std::pair<WorkerId, DeviceId>;

  RenderProgress progressLocked() const;
  void stopLocked();               // no new work, cancel what is in flight, finish
  void serveLocked();              // hand out work to devices that asked for more
  void mergeReadyLocked(uint32_t tile);
  bool mergeLocked(const TileResult& r);
  void checkFinishedLocked();
  void reissueLateLocked();
  double medianItemSecondsLocked() const;

  RenderSettings settings_;
  std::shared_ptr<const Scene> scene_;
  Film& film_;
  Scheduler scheduler_;
  uint32_t sample_begin_ = 0;
  std::vector<std::unique_ptr<WorkerLink>> workers_;
  std::map<DeviceKey, DeviceTally> tallies_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<LeaseId, LeaseInfo> leases_;
  LeaseId next_lease_ = 1;
  std::map<DeviceKey, int> wanted_;                   // devices waiting for this many more items
  std::map<uint64_t, TileResult> early_;              // results waiting for an earlier chunk of their tile
  std::deque<double> item_seconds_;                   // recent item durations, for hang detection
  uint32_t live_devices_ = 0;
  uint32_t reissued_ = 0;
  bool finished_ = false;
  bool out_of_disk_ = false;
  std::string failure_;     // every device failed
  std::string film_error_;  // film write failure
  uint64_t nan_count_ = 0;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace pen
