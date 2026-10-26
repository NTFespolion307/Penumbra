#include "worker/device_slot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>

namespace pen {

namespace {
int64_t nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

double DeviceSlot::busySeconds() const {
  const int64_t start = render_start_us_.load();
  const int64_t current = start < 0 ? 0 : std::max<int64_t>(0, nowMicros() - start);
  return double(busy_us_.load() + uint64_t(current)) * 1e-6;
}

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
    const int want = in_flight_.load();
    if (!finished_ && have < want) {
      lock.unlock();
      cb_.request_work(id_, want - have);
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
    const int64_t start = nowMicros();
    render_start_us_ = start;
    try {
      backend_.render(item, result);
      // Added before the start is cleared: busySeconds() may count this item twice for a moment (Stats clamps).
      busy_us_ += static_cast<uint64_t>(nowMicros() - start);
      render_start_us_ = -1;
    } catch (const std::exception& e) {
      render_start_us_ = -1;
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
