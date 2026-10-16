#include "worker/device_slot.h"

#include <exception>

namespace pen {

DeviceSlot::DeviceSlot(uint32_t id, IBackend& backend, Callbacks callbacks, int inFlight)
    : id_(id), backend_(backend), cb_(std::move(callbacks)), in_flight_(inFlight) {}

DeviceSlot::~DeviceSlot() {
  finish();
  join();
}

void DeviceSlot::start() { thread_ = std::thread([this] { run(); }); }

void DeviceSlot::addLease(const WorkItem& item) {
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(item);
  }
  cv_.notify_one();
}

void DeviceSlot::finish() {
  {
    std::lock_guard lock(mutex_);
    finished_ = true;
  }
  cv_.notify_one();
}

void DeviceSlot::join() {
  if (thread_.joinable()) thread_.join();
}

void DeviceSlot::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    // Keep `in_flight_` items: the one rendering plus queued ones. Requests are answered synchronously
    // in M1 (in-process); a request that yields nothing waits for more leases or finish().
    const int have = static_cast<int>(queue_.size()) + (rendering_ ? 1 : 0);
    if (!finished_ && have < in_flight_) {
      lock.unlock();
      cb_.request_work(id_, in_flight_ - have);
      lock.lock();
    }
    cv_.wait(lock, [&] { return !queue_.empty() || finished_; });
    if (queue_.empty()) break;  // finished and drained

    WorkItem item = queue_.front();
    queue_.pop_front();
    rendering_ = true;
    lock.unlock();
    TileResult result;
    result.device = id_;
    try {
      backend_.render(item, result);
    } catch (const std::exception& e) {
      cb_.failed(id_, e.what());
      lock.lock();
      rendering_ = false;
      break;
    }
    cb_.result(std::move(result));
    lock.lock();
    rendering_ = false;
  }
}

}  // namespace pen
