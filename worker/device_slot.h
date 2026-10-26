#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backends/backend.h"
#include "core/render/work.h"

namespace pen {

// One device on its own thread: asks for work, renders the leases it is given, hands back results
// (docs/GUIDE.md "Per-device flow"). Used by the controller's local worker now and by remote workers in M4.
class DeviceSlot {
 public:
  struct Callbacks {
    // "I can take `count` more items." The controller may answer by calling addLease, now or later.
    std::function<void(uint32_t device, int count)> request_work;
    std::function<void(TileResult&& result)> result;
    // The device threw; it stops for good. `lost` holds the lease it was rendering and the queued ones.
    std::function<void(uint32_t device, const std::string& error, const std::vector<LeaseId>& lost)> failed;
  };

  DeviceSlot(uint32_t id, IBackend& backend, Callbacks callbacks, int inFlight = 2);
  ~DeviceSlot();
  DeviceSlot(const DeviceSlot&) = delete;
  DeviceSlot& operator=(const DeviceSlot&) = delete;

  void start();
  void addLease(const WorkItem& item);
  // Drops queued items that have not started (cancel); the backend's own cancel stops the one in progress.
  void clearQueue();
  // No more work will come: the thread exits once its queue is empty.
  void finish();
  void join();

  uint32_t id() const { return id_; }
  // Items to keep in flight (2, or 3 on links with a long round trip); applies from the next request.
  void setInFlight(int n) { in_flight_ = n; }
  // Time spent inside IBackend::render, for the busy % in Stats, including the item rendering now (a long item
  // would otherwise read 0% until it ends).
  double busySeconds() const;

 private:
  void run();

  uint32_t id_;
  IBackend& backend_;
  Callbacks cb_;
  std::atomic<int> in_flight_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<WorkItem> queue_;
  bool finished_ = false;
  bool rendering_ = false;
  std::atomic<uint64_t> busy_us_{0};
  std::atomic<int64_t> render_start_us_{-1};  // steady clock, while IBackend::render runs; -1 otherwise
  std::thread thread_;
};

}  // namespace pen
