#include "controller/render_controller.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

#include "core/system/file.h"

namespace pen {

namespace {
uint32_t sampleBegin(const RenderSettings& s) { return s.sample_range ? s.sample_range->first : 0; }
uint32_t sampleEnd(const RenderSettings& s) { return s.sample_range ? s.sample_range->last + 1 : s.spp; }
}  // namespace

RenderController::RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene, Film& film)
    : settings_(settings),
      scene_(std::move(scene)),
      film_(film),
      scheduler_(film.layout().width, film.layout().height, film.layout().tile_size, sampleBegin(settings),
                 sampleEnd(settings), settings.region) {}

void RenderController::addWorker(std::unique_ptr<WorkerLink> link) { workers_.push_back(std::move(link)); }

uint32_t RenderController::skipRenderedTiles() {
  std::lock_guard lock(mutex_);
  const uint32_t before = scheduler_.completed();
  samples_done_ += scheduler_.skipDone([&](uint32_t tile) { return film_.tileDone(tile); });
  return scheduler_.completed() - before;
}

void RenderController::onReady(WorkerId worker, const std::vector<DeviceReady>& devices) {
  const WorkerInfo info = workers_[worker]->info();
  std::lock_guard lock(mutex_);
  for (const DeviceReady& d : devices) {
    const std::string name = d.device < info.devices.size() ? info.devices[d.device].name : "?";
    spdlog::info("{} device {} ({}, {}): ready in {:.2f}s, {}", info.name, d.device, name, toString(d.backend),
                 d.build_seconds, d.summary);
    tallies_[{worker, d.device}].name = fmt::format("{}/{} {}", info.name, d.device, name);
  }
}

void RenderController::onRequestWork(WorkerId worker, DeviceId device, int count) {
  std::lock_guard lock(mutex_);
  for (int i = 0; i < count && !stop_issuing_; ++i) {
    const std::optional<WorkItem> item = scheduler_.next();
    if (!item) break;
    workers_[worker]->sendLease({item->id, device, *item});
  }
}

void RenderController::stopLocked() {
  stop_issuing_ = true;
  for (auto& w : workers_) w->cancel({});
  finished_ = true;
  cv_.notify_all();
}

void RenderController::onTileResult(WorkerId worker, TileResult&& r) {
  std::lock_guard lock(mutex_);
  if (finished_) return;  // stopping: late results are not merged
  const auto& items = scheduler_.items();
  const bool known = r.item_id < items.size();
  const WorkItem* item = known ? &items[r.item_id] : nullptr;
  const bool valid = item && item->rect == r.rect && item->sample_begin == r.sample_begin &&
                     item->sample_end == r.sample_end && r.mean.size() == r.rect.pixels();
  if (!valid) {
    spdlog::warn("rejected an invalid result for item {} from worker {}", r.item_id, worker);
    workers_[worker]->ackTile(r.item_id, AckStatus::Rejected);
    if (known) scheduler_.requeue(r.item_id);
    return;
  }
  if (!scheduler_.complete(r.item_id)) {
    workers_[worker]->ackTile(r.item_id, AckStatus::Duplicate);
    return;
  }
  try {
    film_.merge(r);
  } catch (const DiskFullError& e) {
    spdlog::error("out of disk while writing the film: {}", e.what());
    film_error_ = e.what();
    out_of_disk_ = true;
    stopLocked();
    return;
  } catch (const FileError& e) {
    spdlog::error("cannot write the film: {}", e.what());
    film_error_ = e.what();
    stopLocked();
    return;
  }
  workers_[worker]->ackTile(r.item_id, AckStatus::Accepted);
  const uint64_t samples = r.rect.pixels() * (r.sample_end - r.sample_begin);
  samples_done_ += samples;
  if (r.nan_count) {
    nan_count_ += r.nan_count;
    spdlog::warn("item {}: {} non-finite samples replaced by 0", r.item_id, r.nan_count);
  }
  DeviceTally& t = tallies_[{worker, r.device}];
  ++t.tiles;
  t.samples += samples;
  t.seconds += r.render_seconds;
  if (scheduler_.done() || (stop_issuing_ && scheduler_.outstanding() == 0)) {
    finished_ = true;
    cv_.notify_all();
  }
}

void RenderController::onRelease(WorkerId, std::span<const LeaseId> leases) {
  std::lock_guard lock(mutex_);
  for (LeaseId id : leases) {
    spdlog::info("item {} released; requeued", id);
    scheduler_.requeue(id);
  }
}

void RenderController::onGone(WorkerId worker, GoneReason reason, const std::string& detail) {
  std::lock_guard lock(mutex_);
  if (reason == GoneReason::Failed) {
    // M2 has a single local worker, so a device failure ends the render; M3 requeues its items instead.
    spdlog::error("worker {} failed: {}", worker, detail);
    failure_ = detail;
    finished_ = true;
    cv_.notify_all();
  }
}

RenderProgress RenderController::progressLocked() const {
  RenderProgress p;
  p.samples_done = samples_done_;
  p.samples_total = scheduler_.totalSamples();
  p.items_done = scheduler_.completed();
  p.items_total = scheduler_.total();
  p.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  return p;
}

RenderProgress RenderController::progress() const {
  std::lock_guard lock(mutex_);
  return progressLocked();
}

RenderOutcome RenderController::run(const RenderRunOptions& options) {
  start_ = std::chrono::steady_clock::now();
  RenderOutcome outcome;
  {
    std::lock_guard lock(mutex_);
    if (scheduler_.done()) finished_ = true;  // resumed a film that was already complete
  }
  if (!finished_)
    for (auto& w : workers_) w->offerScene(scene_, settings_);

  auto lastProgress = std::chrono::steady_clock::now();
  auto lastCheckpoint = lastProgress;
  {
    std::unique_lock lock(mutex_);
    while (!finished_) {
      cv_.wait_for(lock, std::chrono::milliseconds(200));
      if (finished_) break;
      const auto now = std::chrono::steady_clock::now();
      const RenderProgress p = progressLocked();

      if (options.interrupt && options.interrupt->load()) {
        spdlog::warn("interrupted: stopping; tiles in progress are discarded and the film is checkpointed");
        outcome.interrupted = true;
        stopLocked();
        break;
      }
      if (settings_.time_limit_s > 0 && !stop_issuing_ && p.elapsed_seconds >= settings_.time_limit_s) {
        stop_issuing_ = true;
        outcome.time_limited = true;
        spdlog::info("time limit reached: no new work; waiting for {} item(s) in progress", scheduler_.outstanding());
        if (scheduler_.outstanding() == 0) finished_ = true;
      }
      if (std::chrono::duration<double>(now - lastCheckpoint).count() >= settings_.checkpoint_s) {
        lastCheckpoint = now;
        try {
          film_.checkpoint();
          spdlog::info("checkpoint: {} of {} tiles in the film", film_.tilesDone(), film_.layout().tileCount());
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
  for (auto& w : workers_) w->complete();

  std::lock_guard lock(mutex_);
  if (!failure_.empty()) throw std::runtime_error(failure_);
  if (options.progress) options.progress(progressLocked());
  outcome.complete = scheduler_.done();
  outcome.out_of_disk = out_of_disk_;
  outcome.error = film_error_;
  outcome.nan_count = nan_count_;
  outcome.seconds = progressLocked().elapsed_seconds;
  for (const auto& [key, tally] : tallies_) outcome.devices.push_back(tally);
  return outcome;
}

}  // namespace pen
