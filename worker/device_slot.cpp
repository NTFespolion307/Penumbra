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

void DeviceSlot::clearQueue() {
  std::lock_guard lock(mutex_);
  queue_.clear();
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
    // Keep `in_flight_` items: the one rendering plus queued ones (docs/GUIDE.md "Leases": 2 per slot).
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
      lock.lock();
      std::vector<LeaseId> lost{item.lease};
      for (const WorkItem& q : queue_) lost.push_back(q.lease);
      queue_.clear();
      rendering_ = false;
      finished_ = true;
      lock.unlock();
      cb_.failed(id_, e.what(), lost);
      return;
    }
    result.lease = item.lease;
    if (!result.cancelled) cb_.result(std::move(result));  // cancelled results are incomplete: drop them
    lock.lock();
    rendering_ = false;
  }
}

}  // namespace pen
