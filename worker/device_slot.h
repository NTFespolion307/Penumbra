#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "backends/backend.h"
#include "core/render/work.h"

namespace pen {

// One device on its own thread: asks for work, renders the items it is given, hands back results
// (docs/GUIDE.md "Per-device flow"). Used by the controller's local worker now and by remote workers in M4.
class DeviceSlot {
 public:
  struct Callbacks {
    std::function<void(uint32_t device, int count)> request_work;  // may call addLease before returning
    std::function<void(TileResult&& result)> result;
    std::function<void(uint32_t device, const std::string& error)> failed;
  };

  DeviceSlot(uint32_t id, IBackend& backend, Callbacks callbacks, int inFlight = 2);
  ~DeviceSlot();
  DeviceSlot(const DeviceSlot&) = delete;
  DeviceSlot& operator=(const DeviceSlot&) = delete;

  void start();
  void addLease(const WorkItem& item);
  // No more work will come: the thread exits once its queue is empty.
  void finish();
  void join();

  uint32_t id() const { return id_; }

 private:
  void run();

  uint32_t id_;
  IBackend& backend_;
  Callbacks cb_;
  int in_flight_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<WorkItem> queue_;
  bool finished_ = false;
  bool rendering_ = false;
  std::thread thread_;
};

}  // namespace pen
