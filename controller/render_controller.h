#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
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
  double seconds = 0;  // inside the backend's render calls
  // Its render window, in seconds since the render started: the first item's start (its result time minus its
  // render time) to the last result. Busy % = seconds / window, without the scene upload (--benchmark, M9).
  double first_begin = -1, last_end = 0;
  BackendKind backend = BackendKind::Cpu;
  double ready_seconds = 0;  // scene upload and build on the device
  std::string placement;     // memory tier summary
  bool failed = false;
  bool left = false;  // its worker disconnected
};

struct RenderOutcome {
  bool complete = false;      // every chunk of every tile merged
  bool time_limited = false;  // stopped by --time-limit
  bool interrupted = false;   // stopped by Ctrl+C
  bool out_of_disk = false;   // a film write failed for lack of space
  std::string error;          // film write error text, if any
  uint64_t nan_count = 0;
  uint32_t reissued = 0;      // leases handed out again (late or lost)
  uint32_t duplicated = 0;    // tail duplicates handed out (DECISIONS D-091)
  uint32_t duplicate_wins = 0;  // ... whose result arrived first
  // Controller throughput (M9, D-112): results received, time spent unpacking them (network thread) and merging them
  // (under the controller lock).
  uint64_t results = 0;
  double decode_seconds = 0, merge_seconds = 0;
  double serve_seconds = 0, late_seconds = 0;  // handing out work, the late-lease check (under the lock)
  double tail_seconds = 0;                     // ... of which looking for tail duplicates
  double network_seconds = 0;                  // the network threads busy with received data
  double tls_seconds = 0;                      // ... of which decrypting it
  double seconds = 0;
  std::vector<DeviceTally> devices;
  RenderProgress progress;  // at the end, for the session file
};

struct RenderRunOptions {
  const std::atomic<bool>* interrupt = nullptr;                 // set by the Ctrl+C handler
  std::function<void(const RenderProgress&)> progress;          // about once per second
  std::function<void(const RenderProgress&)> after_checkpoint;  // after each periodic film checkpoint
  uint32_t live_view = 0;  // UI viewport: keep a downsampled copy of the film, this many pixels on the long side
};

// What the UI's viewport shows (docs/GUIDE.md "Viewport page"): the film downsampled to at most `live_view` pixels on
// the long side, sample-weighted exactly like the film, and which worker merged each tile last.
struct LiveView {
  uint32_t width = 0, height = 0;  // of `rgb`
  std::vector<Vec3f> rgb;          // linear radiance, rows top-down; black where nothing merged yet
  FilmLayout layout;               // of the full film
  std::vector<int32_t> tile_owner;   // per film tile: the worker that merged its last chunk, -1 none
  std::vector<uint8_t> tile_busy;    // per film tile: a lease is out for it
  std::vector<std::string> workers;  // worker names by WorkerId
  uint64_t version = 0;              // grows with every merge
};

// Runs one render (docs/GUIDE.md "Render lifecycle", steps 3-8; "Scheduling, leases and tiles"): hands work
// items to device slots as leases, merges each tile's chunks into the film strictly in chunk order (so any
// distribution of work gives the same bits, D-045), re-issues leases that take more than 10x the median item
// time, requeues the leases of failed devices, and checkpoints the film every --checkpoint. When the queue is
// empty, an idle device gets a copy of the oldest outstanding item it can finish sooner (tail duplication,
// D-091); the first valid result wins.
class RenderController final : public WorkerEvents {
 public:
  // aovFilms[pass]: the film of each AOV pass to render (M11, D-127), nullptr for passes not wanted.
  RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene,
                   std::shared_ptr<const ScenePackage> package, Film& film, PassOrder order,
                   const std::array<Film*, kPassCount>& aovFilms = {});

  // `make` creates the link for the new worker id, with this controller as its WorkerEvents. Thread-safe and
  // allowed while the render runs: a worker added then is offered the scene at once (joining mid-render).
  WorkerId addWorker(const std::function<std::unique_ptr<WorkerLink>(WorkerId)>& make);

  // Hosting: workers may come and go, so a render with no devices left waits for the next worker instead of
  // failing.
  void setHosting(bool hosting) { hosting_ = hosting; }

  // Resume: continue every tile from the samples already in the film. Returns tiles with data.
  uint32_t restoreFromFilm();

  RenderOutcome run(const RenderRunOptions& options);
  RenderProgress progress() const;

  // UI viewport (needs RenderRunOptions::live_view): copies the live view if it changed since `version`.
  bool liveView(LiveView& out, uint64_t version) const;
  // Pause: no new work is handed out and workers are told to stop asking; leases in flight finish (docs/GUIDE.md
  // "Viewport page": Pause, Resume).
  void setPaused(bool paused);
  bool paused() const;
  // "Save image now": writes the film as it is to `settings.output` while the render goes on (merges wait).
  // Returns false with the reason.
  bool writeImageNow(const RenderSettings& settings, std::string& error);
  // Full-resolution pixels of `rect` from the film (the viewport zoomed past the live view's resolution).
  void readFilm(const TileRect& rect, std::vector<Vec3f>& out) const;

  void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) override;
  // What `worker`'s devices reported when they took the scene (memory tier, placement), empty before that.
  std::vector<DeviceReady> deviceReadiness(WorkerId worker) const;
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
    bool duplicate = false;   // a tail copy of an item another lease holds
    bool duplicated = false;  // this lease's item has a tail copy out
  };
  using DeviceKey = std::pair<WorkerId, DeviceId>;

  RenderProgress progressLocked() const;
  void stopLocked();               // no new work, cancel what is in flight, finish
  void serveLocked();              // hand out work to devices that asked for more
  void mergeReadyLocked(uint32_t tile, Pass pass);
  bool mergeLocked(const TileResult& r);
  void checkFinishedLocked();
  void reissueLateLocked();
  // What one serveLocked() call learns about the leases while looking for tail duplicates, so it scans them once,
  // not once per waiting device (with 512 devices and 1,500 leases that saturated the network thread, D-112).
  struct TailScan {
    bool built = false;
    std::set<std::pair<WorkerId, DeviceId>> busy;  // devices holding a lease
    double failed_mine = std::numeric_limits<double>::infinity();  // a device this slow or slower found nothing
    bool nothing = false;                          // no lease can be copied at all
  };
  std::optional<WorkItem> tailDuplicateLocked(const std::pair<WorkerId, DeviceId>& device, TailScan& scan);
  bool otherLeaseHoldsLocked(uint64_t item, LeaseId except) const;
  void requeueLostLocked(LeaseId id, const LeaseInfo& lease);
  void requeueWorkerLocked(WorkerId worker);
  std::vector<WorkerLink*> workerSnapshotLocked() const;
  static double medianSeconds(const std::deque<double>& window);  // -1 with fewer than 3 items

  RenderSettings settings_;
  RenderOffer offer_;
  Film& film_;
  std::array<Film*, kPassCount> films_{};  // by pass: [0] the image (film_), then the AOVs being rendered
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
  std::map<WorkerId, uint32_t> rejects_;              // invalid results per worker
  std::map<WorkerId, std::vector<DeviceReady>> ready_;  // each worker's devices: memory tier and placement
  std::deque<double> item_seconds_;                   // recent item durations, for hang detection
  std::map<DeviceKey, std::deque<double>> device_seconds_;  // the same per device (D-097)
  std::map<DeviceKey, std::chrono::steady_clock::time_point> device_last_result_;  // when each last delivered (D-111)
  uint32_t live_devices_ = 0;
  bool hosting_ = false;
  bool running_ = false;  // run() has offered the scene; later workers get it from addWorker
  uint32_t reissued_ = 0;
  uint32_t duplicated_ = 0, duplicate_wins_ = 0;
  uint32_t merged_items_ = 0;
  uint64_t results_ = 0;
  double decode_seconds_ = 0, merge_seconds_ = 0, serve_seconds_ = 0, late_seconds_ = 0, tail_seconds_ = 0;
  double network_start_ = 0, tls_start_ = 0;  // networkBusySeconds(), networkTlsSeconds() when run() started
  bool paused_ = false;
  // Live view (UI): per downsampled pixel, the sample-weighted sum and the samples.
  uint32_t live_w_ = 0, live_h_ = 0;
  std::vector<Vec3f> live_sum_;
  std::vector<double> live_weight_;
  std::vector<int32_t> tile_owner_;
  std::map<uint64_t, WorkerId> early_owner_;  // the worker of each accepted result until it is merged
  uint64_t live_version_ = 0;
  void liveMergeLocked(const TileResult& r, WorkerId worker);
  void liveInitLocked(uint32_t maxSide);
  bool finished_ = false;
  bool out_of_disk_ = false;
  std::string failure_;     // every device failed
  std::string film_error_;  // film write failure
  uint64_t nan_count_ = 0;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace pen
