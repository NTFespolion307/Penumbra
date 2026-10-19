#include "controller/render_controller.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

#include "core/system/file.h"

namespace pen {

namespace {
using Clock = std::chrono::steady_clock;
uint32_t sampleBegin(const RenderSettings& s) { return s.sample_range ? s.sample_range->first : 0; }
uint32_t sampleEnd(const RenderSettings& s) {
  if (s.sample_range) return s.sample_range->last + 1;
  return s.spp == 0 ? kUnboundedSamples : s.spp;
}
constexpr double kMinLateSeconds = 2.0;  // never call a lease late before this
constexpr size_t kDurationWindow = 256;
}  // namespace

RenderController::RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene, Film& film,
                                   PassOrder order)
    : settings_(settings),
      scene_(std::move(scene)),
      film_(film),
      scheduler_(film.layout(), sampleBegin(settings), sampleEnd(settings), settings.region, order),
      sample_begin_(sampleBegin(settings)) {}

WorkerId RenderController::addWorker(const std::function<std::unique_ptr<WorkerLink>(WorkerId)>& make) {
  WorkerLink* link = nullptr;
  WorkerId id = 0;
  bool offer = false;
  {
    std::lock_guard lock(mutex_);
    id = static_cast<WorkerId>(workers_.size());
    workers_.push_back(make(id));
    link = workers_.back().get();
    offer = running_ && !finished_;
  }
  if (offer) link->offerScene(scene_, settings_);  // joined mid-render: gets the scene like everyone else
  return id;
}

std::vector<WorkerLink*> RenderController::workerSnapshotLocked() const {
  std::vector<WorkerLink*> v;
  for (const auto& w : workers_) v.push_back(w.get());
  return v;
}

uint32_t RenderController::restoreFromFilm() {
  std::lock_guard lock(mutex_);
  scheduler_.restore([&](uint32_t tile) { return film_.tileSamples(tile); });
  return film_.tilesWithData();
}

// ---------------------------------------------------------------- worker events

void RenderController::onReady(WorkerId worker, const std::vector<DeviceReady>& devices) {
  std::lock_guard lock(mutex_);
  const WorkerInfo info = workers_[worker]->info();
  for (const DeviceReady& d : devices) {
    const std::string name = d.device < info.devices.size() ? info.devices[d.device].name : "?";
    spdlog::info("{} device {} ({}, {}): ready in {:.2f}s, {}", info.name, d.device, name, toString(d.backend),
                 d.build_seconds, d.summary);
    tallies_[{worker, d.device}].name = fmt::format("{}/{} {}", info.name, d.device, name);
    ++live_devices_;
  }
}

void RenderController::onRequestWork(WorkerId worker, DeviceId device, int count) {
  std::lock_guard lock(mutex_);
  const DeviceTally& t = tallies_[{worker, device}];
  if (finished_ || t.failed || t.left) return;
  wanted_[{worker, device}] = count;  // "I can take this many more", not cumulative
  serveLocked();
}

void RenderController::serveLocked() {
  for (auto it = wanted_.begin(); it != wanted_.end();) {
    auto& [key, want] = *it;
    while (want > 0) {
      std::optional<WorkItem> item = scheduler_.next();
      if (!item) return;  // nothing to hand out now; devices keep waiting
      item->lease = next_lease_++;
      leases_[item->lease] = {*item, key.first, key.second, Clock::now(), false};
      --want;
      workers_[key.first]->sendLease({item->lease, key.second, *item});
    }
    it = wanted_.erase(it);
  }
}

bool RenderController::mergeLocked(const TileResult& r) {
  try {
    film_.merge(r, r.sample_end - sample_begin_);
  } catch (const DiskFullError& e) {
    spdlog::error("out of disk while writing the film: {}", e.what());
    film_error_ = e.what();
    out_of_disk_ = true;
    stopLocked();
    return false;
  } catch (const FileError& e) {
    spdlog::error("cannot write the film: {}", e.what());
    film_error_ = e.what();
    stopLocked();
    return false;
  }
  const uint32_t tile = static_cast<uint32_t>(r.item_id >> 32);
  scheduler_.markMerged(tile);
  if (scheduler_.tileComplete(tile)) {
    try {
      film_.flushTile(tile);
    } catch (const FileError& e) {
      spdlog::error("cannot write the film: {}", e.what());
      film_error_ = e.what();
      out_of_disk_ = dynamic_cast<const DiskFullError*>(&e) != nullptr;
      stopLocked();
      return false;
    }
  }
  return true;
}

void RenderController::mergeReadyLocked(uint32_t tile) {
  for (;;) {
    const uint64_t id = WorkItem::makeId(tile, scheduler_.mergedChunks(tile));
    auto it = early_.find(id);
    if (it == early_.end()) return;
    TileResult r = std::move(it->second);
    early_.erase(it);
    if (!mergeLocked(r)) return;
  }
}

void RenderController::onTileResult(WorkerId worker, TileResult&& r) {
  std::lock_guard lock(mutex_);
  if (finished_) return;  // stopping: late results are not merged
  auto lease = leases_.find(r.lease);
  if (lease == leases_.end()) {
    workers_[worker]->ackTile(r.lease, AckStatus::Rejected);  // cancelled or unknown lease
    return;
  }
  const WorkItem item = lease->second.item;
  const double seconds = std::chrono::duration<double>(Clock::now() - lease->second.issued).count();
  leases_.erase(lease);

  const bool valid = item.id == r.item_id && item.rect == r.rect && item.sample_begin == r.sample_begin &&
                     item.sample_end == r.sample_end && r.mean.size() == r.rect.pixels();
  if (!valid) {
    spdlog::warn("rejected an invalid result for lease {} from worker {}; item requeued", r.lease, worker);
    workers_[worker]->ackTile(r.lease, AckStatus::Rejected);
    scheduler_.requeue(item);
    serveLocked();
    return;
  }
  item_seconds_.push_back(seconds);
  if (item_seconds_.size() > kDurationWindow) item_seconds_.pop_front();

  // Already merged (a late copy of a re-issued item) or already waiting: the first valid result won.
  if (item.chunk < scheduler_.mergedChunks(item.tile) || early_.count(item.id)) {
    workers_[worker]->ackTile(r.lease, AckStatus::Duplicate);
    checkFinishedLocked();
    return;
  }
  workers_[worker]->ackTile(r.lease, AckStatus::Accepted);
  if (r.nan_count) {
    nan_count_ += r.nan_count;
    spdlog::warn("tile {} chunk {}: {} non-finite samples replaced by 0", item.tile, item.chunk, r.nan_count);
  }
  DeviceTally& t = tallies_[{worker, r.device}];
  ++t.items;
  t.samples += r.rect.pixels() * (r.sample_end - r.sample_begin);
  t.seconds += r.render_seconds;

  early_.emplace(item.id, std::move(r));
  mergeReadyLocked(item.tile);
  checkFinishedLocked();
}

void RenderController::checkFinishedLocked() {
  if (finished_) return;
  if (scheduler_.done() || (scheduler_.stopped() && leases_.empty())) {
    finished_ = true;
    cv_.notify_all();
  }
}

void RenderController::onRelease(WorkerId worker, std::span<const LeaseId> released) {
  std::lock_guard lock(mutex_);
  for (LeaseId id : released) {
    auto it = leases_.find(id);
    if (it == leases_.end()) continue;
    spdlog::info("lease {} (tile {} chunk {}) released by worker {}; requeued", id, it->second.item.tile,
                 it->second.item.chunk, worker);
    if (!it->second.reissued) scheduler_.requeue(it->second.item);
    ++reissued_;
    leases_.erase(it);
  }
  serveLocked();
  checkFinishedLocked();
}

void RenderController::onDeviceFailed(WorkerId worker, DeviceId device, const std::string& error) {
  std::lock_guard lock(mutex_);
  DeviceTally& t = tallies_[{worker, device}];
  if (t.failed) return;
  t.failed = true;
  wanted_.erase({worker, device});
  spdlog::error("device {} of worker {} failed and is removed: {}", device, worker, error);
  if (live_devices_ > 0) --live_devices_;
  if (live_devices_ == 0 && hosting_) {
    spdlog::warn("no devices left; waiting for a worker to join");
  } else if (live_devices_ == 0) {
    failure_ = fmt::format("every device failed (last: {})", error);
    stopLocked();
  }
}

void RenderController::requeueWorkerLocked(WorkerId worker) {
  for (auto it = leases_.begin(); it != leases_.end();) {
    if (it->second.worker != worker) {
      ++it;
      continue;
    }
    if (!it->second.reissued) scheduler_.requeue(it->second.item);
    ++reissued_;
    it = leases_.erase(it);
  }
  for (auto it = wanted_.begin(); it != wanted_.end();) it = it->first.first == worker ? wanted_.erase(it) : std::next(it);
}

void RenderController::onGone(WorkerId worker, GoneReason reason, const std::string& detail) {
  std::lock_guard lock(mutex_);
  if (reason == GoneReason::Failed && !hosting_) {
    spdlog::error("worker {} failed: {}", worker, detail);
    failure_ = detail;
    stopLocked();
    return;
  }
  size_t outstanding = 0;
  for (const auto& [id, info] : leases_) outstanding += info.worker == worker;
  if (!finished_)
    spdlog::info("worker {} left ({}); {} lease(s) requeued", worker, detail, outstanding);
  requeueWorkerLocked(worker);
  for (auto& [key, tally] : tallies_) {
    if (key.first != worker || tally.failed || tally.left) continue;
    tally.left = true;
    if (live_devices_ > 0) --live_devices_;
  }
  if (live_devices_ == 0 && !finished_ && !hosting_) {
    failure_ = fmt::format("every worker left (last: {})", detail);
    stopLocked();
    return;
  }
  if (live_devices_ == 0 && !finished_) spdlog::warn("no devices left; waiting for a worker to join");
  serveLocked();
  checkFinishedLocked();
}

// ---------------------------------------------------------------- run loop

void RenderController::stopLocked() {
  scheduler_.stopNew();
  for (auto& w : workers_) w->cancel({});  // non-blocking for every link
  finished_ = true;
  cv_.notify_all();
}

double RenderController::medianItemSecondsLocked() const {
  if (item_seconds_.size() < 3) return -1;
  std::vector<double> v(item_seconds_.begin(), item_seconds_.end());
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

void RenderController::reissueLateLocked() {
  const double median = medianItemSecondsLocked();
  if (median < 0) return;
  const double limit = std::max(10 * median, kMinLateSeconds);
  const auto now = Clock::now();
  bool any = false;
  for (auto& [id, info] : leases_) {
    if (info.reissued) continue;
    const double age = std::chrono::duration<double>(now - info.issued).count();
    if (age <= limit) continue;
    spdlog::warn("lease {} (tile {} chunk {}) on device {} of worker {} is late ({:.1f}s, 10x median is {:.1f}s); "
                 "handing it out again, the first valid result wins",
                 id, info.item.tile, info.item.chunk, info.device, info.worker, age, 10 * median);
    info.reissued = true;
    ++reissued_;
    scheduler_.requeue(info.item);
    any = true;
  }
  if (any) serveLocked();
}

RenderProgress RenderController::progressLocked() const {
  RenderProgress p;
  p.samples_done = scheduler_.mergedSamples();
  p.samples_total = scheduler_.totalSamples();
  p.items_done = scheduler_.tilesComplete();
  p.items_total = scheduler_.tileCount();
  p.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start_).count();
  p.time_limit_seconds = static_cast<double>(settings_.time_limit_s);
  return p;
}

RenderProgress RenderController::progress() const {
  std::lock_guard lock(mutex_);
  return progressLocked();
}

RenderOutcome RenderController::run(const RenderRunOptions& options) {
  start_ = Clock::now();
  RenderOutcome outcome;
  std::vector<WorkerLink*> initial;
  {
    std::lock_guard lock(mutex_);
    if (scheduler_.done()) finished_ = true;  // resumed a film that was already complete
    running_ = true;
    initial = workerSnapshotLocked();
  }
  if (!finished_)
    for (WorkerLink* w : initial) w->offerScene(scene_, settings_);

  auto lastProgress = Clock::now();
  auto lastCheckpoint = lastProgress;
  {
    std::unique_lock lock(mutex_);
    while (!finished_) {
      cv_.wait_for(lock, std::chrono::milliseconds(200));
      if (finished_) break;
      const auto now = Clock::now();
      const RenderProgress p = progressLocked();

      if (options.interrupt && options.interrupt->load()) {
        spdlog::warn("interrupted: stopping; chunks in progress are discarded and the film is checkpointed");
        outcome.interrupted = true;
        stopLocked();
        break;
      }
      if (settings_.time_limit_s > 0 && !scheduler_.stopped() && p.elapsed_seconds >= settings_.time_limit_s) {
        scheduler_.stopNew();
        outcome.time_limited = true;
        spdlog::info("time limit reached: no new work; waiting for {} lease(s) in progress", leases_.size());
        checkFinishedLocked();
        if (finished_) break;
      }
      reissueLateLocked();
      if (std::chrono::duration<double>(now - lastCheckpoint).count() >= settings_.checkpoint_s) {
        lastCheckpoint = now;
        try {
          film_.checkpoint();
          spdlog::info("checkpoint: {} of {} tiles finished, {} with data", scheduler_.tilesComplete(),
                       scheduler_.tileCount(), film_.tilesWithData());
        } catch (const FileError& e) {
          spdlog::error("checkpoint failed: {}", e.what());
          film_error_ = e.what();
          out_of_disk_ = dynamic_cast<const DiskFullError*>(&e) != nullptr;
          stopLocked();
          break;
        }
        if (options.after_checkpoint) {
          lock.unlock();
          options.after_checkpoint(p);
          lock.lock();
        }
      }
      if (options.progress && std::chrono::duration<double>(now - lastProgress).count() >= 1.0) {
        lastProgress = now;
        lock.unlock();
        options.progress(p);
        lock.lock();
      }
    }
  }
  // Wake devices that are still busy (a hung device waits for cancel), then let every worker finish.
  std::vector<WorkerLink*> all;
  {
    std::lock_guard lock(mutex_);
    all = workerSnapshotLocked();
  }
  for (WorkerLink* w : all) w->cancel({});
  for (WorkerLink* w : all) w->complete();

  std::lock_guard lock(mutex_);
  if (!failure_.empty()) throw std::runtime_error(failure_);
  if (options.progress) options.progress(progressLocked());
  outcome.complete = scheduler_.done();
  outcome.out_of_disk = out_of_disk_;
  outcome.error = film_error_;
  outcome.nan_count = nan_count_;
  outcome.reissued = reissued_;
  outcome.seconds = progressLocked().elapsed_seconds;
  for (const auto& [key, tally] : tallies_) outcome.devices.push_back(tally);
  return outcome;
}

}  // namespace pen
