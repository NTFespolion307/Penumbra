#include "controller/render_controller.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include "core/output/output.h"
#include "core/system/file.h"
#include "core/system/paths.h"
#include "core/system/test_hooks.h"
#include "net/stats.h"

namespace pen {

namespace {
using Clock = std::chrono::steady_clock;
uint32_t sampleBegin(const RenderSettings& s) { return s.sample_range ? s.sample_range->first : 0; }
uint32_t sampleEnd(const RenderSettings& s) {
  if (s.sample_range) return s.sample_range->last + 1;
  return s.spp == 0 ? kUnboundedSamples : s.spp;
}
constexpr double kMinLateSeconds = 2.0;  // never call a lease late before this
constexpr auto kFirstItemsGrace = std::chrono::seconds(30);  // ...nor a device's first items before this (D-109)
constexpr size_t kDurationWindow = 256;
constexpr size_t kDeviceDurationWindow = 32;
constexpr uint32_t kMaxRejects = 3;

// Test only: turns tail duplication off so the tests of late leases see a hung device being caught (D-091).
bool tailDuplicationEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("PENUMBRA_TEST_NO_TAIL_DUPLICATION");
    if (v && *v && std::string_view(v) != "0") {
      spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_NO_TAIL_DUPLICATION): no tail duplication");
      return false;
    }
    return true;
  }();
  return enabled;
}

// Test only (D-095): once this many chunks are merged, checkpoint the film and session, then exit at once with
// code 137 like a killed process, so resume tests cut a render at the same point on any machine. 0 = off.
uint32_t killAfterItems() {
  static const uint32_t n = [] {
    const char* v = std::getenv("PENUMBRA_TEST_KILL_AFTER_ITEMS");
    const uint32_t k = v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0;
    if (k > 0) spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_KILL_AFTER_ITEMS): exiting after {} merged chunks", k);
    return k;
  }();
  return n;
}
}  // namespace

namespace {
// The AOV passes that have a film, with their sample counts (aovPassesFor).
std::vector<AovPass> wantedAovs(const RenderSettings& s, const std::array<Film*, kPassCount>& films) {
  std::vector<AovPass> v;
  for (const AovPass& a : aovPassesFor(s))
    if (films[size_t(a.pass)]) v.push_back(a);
  return v;
}
}  // namespace

RenderController::RenderController(const RenderSettings& settings, std::shared_ptr<const Scene> scene,
                                   std::shared_ptr<const ScenePackage> package, Film& film, PassOrder order,
                                   const std::array<Film*, kPassCount>& aovFilms)
    : settings_(settings),
      offer_{std::move(scene), std::move(package), settings},
      film_(film),
      films_(aovFilms),
      scheduler_(film.layout(), sampleBegin(settings), sampleEnd(settings), settings.region, order,
                 wantedAovs(settings, aovFilms)),
      sample_begin_(sampleBegin(settings)) {
  films_[0] = &film_;
}

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
  if (offer) link->offerScene(offer_);  // joined mid-render: gets the scene like everyone else
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
  for (uint32_t p = 1; p < kPassCount; ++p)
    if (films_[p] && scheduler_.hasPass(Pass(p)))
      scheduler_.restore([&](uint32_t tile) { return films_[p]->tileSamples(tile); }, Pass(p));
  return film_.tilesWithData();
}

// ---------------------------------------------------------------- worker events

std::vector<DeviceReady> RenderController::deviceReadiness(WorkerId worker) const {
  std::lock_guard lock(mutex_);
  const auto it = ready_.find(worker);
  return it == ready_.end() ? std::vector<DeviceReady>{} : it->second;
}

void RenderController::onReady(WorkerId worker, const std::vector<DeviceReady>& devices) {
  std::lock_guard lock(mutex_);
  ready_[worker] = devices;
  const WorkerInfo info = workers_[worker]->info();
  for (const DeviceReady& d : devices) {
    const std::string name = d.device < info.devices.size() ? info.devices[d.device].name : "?";
    if (d.skipped) {
      spdlog::warn("{} device {} ({}, {}) is skipped for this render: {}", info.name, d.device, name,
                   toString(d.backend), d.reason);
      continue;
    }
    spdlog::info("{} device {} ({}, {}): ready in {:.2f}s, {}", info.name, d.device, name, toString(d.backend),
                 d.build_seconds, d.summary);
    DeviceTally& t = tallies_[{worker, d.device}];
    t.name = fmt::format("{}/{} {}", info.name, d.device, name);
    t.backend = d.backend;
    t.ready_seconds = d.build_seconds;
    t.placement = d.summary;
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
  if (paused_) return;  // devices keep their requests and get work on Resume
  TailScan scan;
  struct Timed {
    double& total;
    Clock::time_point start = Clock::now();
    ~Timed() { total += std::chrono::duration<double>(Clock::now() - start).count(); }
  } timed{serve_seconds_};
  for (auto it = wanted_.begin(); it != wanted_.end();) {
    auto& [key, want] = *it;
    while (want > 0) {
      std::optional<WorkItem> item = scheduler_.next(workers_[key.first]->itemSpan());
      const bool duplicate = !item;
      if (!item) item = tailDuplicateLocked(key, scan);
      if (!item) break;  // nothing for this device now; it keeps waiting
      item->lease = next_lease_++;
      leases_[item->lease] = {*item, key.first, key.second, Clock::now(), false, duplicate};
      --want;
      workers_[key.first]->sendLease({item->lease, key.second, *item});
    }
    it = want > 0 ? std::next(it) : wanted_.erase(it);
  }
}

// docs/GUIDE.md "End of render": the queue is empty, so an idle device takes a copy of the oldest outstanding item
// of another device, if by the devices' mean item times it should finish first. Each item gets one copy at most.
std::optional<WorkItem> RenderController::tailDuplicateLocked(const DeviceKey& device, TailScan& scan) {
  if (!tailDuplicationEnabled() || finished_) return std::nullopt;
  struct Timed {
    double& total;
    Clock::time_point start = Clock::now();
    ~Timed() { total += std::chrono::duration<double>(Clock::now() - start).count(); }
  } timed{tail_seconds_};
  if (scan.nothing) return std::nullopt;
  if (!scan.built) {
    for (const auto& [id, l] : leases_) scan.busy.insert({l.worker, l.device});
    scan.built = true;
  }
  if (scan.busy.count(device)) return std::nullopt;  // not idle
  const auto meanSeconds = [&](const DeviceKey& k) {
    auto t = tallies_.find(k);
    return t != tallies_.end() && t->second.items > 0 ? t->second.seconds / t->second.items : -1.0;
  };
  const double mine = meanSeconds(device);
  // A device at least as slow as one that found nothing finds nothing either (the skip below only grows with `mine`).
  if (mine >= 0 && mine >= scan.failed_mine) return std::nullopt;
  const auto now = Clock::now();
  // Lease ids grow with time, so the first copyable lease in id order is the oldest.
  LeaseId best = 0;
  for (const auto& [id, l] : leases_) {
    if (l.duplicate || l.duplicated || l.reissued) continue;
    const double theirs = meanSeconds({l.worker, l.device});
    const double age = std::chrono::duration<double>(now - l.issued).count();
    // The owner should finish first: it is not overdue yet and what it has left is less than a whole item here.
    if (mine >= 0 && theirs >= 0 && age < theirs && mine >= theirs - age) continue;
    best = id;
    break;
  }
  if (best == 0) {
    if (mine < 0) scan.nothing = true;  // no device skips anything more than one without a mean
    else scan.failed_mine = std::min(scan.failed_mine, mine);
    return std::nullopt;
  }
  scan.busy.insert(device);  // it gets the copy
  LeaseInfo& orig = leases_[best];
  orig.duplicated = true;
  ++duplicated_;
  spdlog::info("tail: lease {} (tile {} chunk {}) of device {} of worker {} duplicated to device {} of worker {}; "
               "the first valid result wins",
               best, orig.item.tile, orig.item.chunk, orig.device, orig.worker, device.second, device.first);
  return orig.item;
}

bool RenderController::otherLeaseHoldsLocked(uint64_t item, LeaseId except) const {
  for (const auto& [id, l] : leases_)
    if (id != except && l.item.id == item) return true;
  return false;
}

// A lease that will bring no result: its item goes back to the queue unless a copy is queued (late) or another
// lease still holds it (tail duplicate).
void RenderController::requeueLostLocked(LeaseId id, const LeaseInfo& lease) {
  if (!lease.reissued && !otherLeaseHoldsLocked(lease.item.id, id)) scheduler_.requeue(lease.item);
}

bool RenderController::mergeLocked(const TileResult& r) {
  const Pass pass = WorkItem::idPass(r.item_id);
  Film& film = *films_[size_t(pass)];
  try {
    // An AOV pass starts at sample 0; the image at --sample-range's first sample.
    film.merge(r, r.sample_end - (pass == Pass::Beauty ? sample_begin_ : 0));
    if (auto o = early_owner_.find(r.item_id); o != early_owner_.end()) {
      if (live_w_ && pass == Pass::Beauty) liveMergeLocked(r, o->second);
      early_owner_.erase(o);
    }
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
  const uint32_t tile = WorkItem::idTile(r.item_id);
  scheduler_.markMergedUntil(tile, r.sample_end, pass);
  ++merged_items_;
  if (scheduler_.tileComplete(tile, pass)) {
    try {
      film.flushTile(tile);
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

void RenderController::mergeReadyLocked(uint32_t tile, Pass pass) {
  for (;;) {
    const uint64_t id = WorkItem::makeId(tile, scheduler_.mergedChunks(tile, pass), pass);
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
  // Time under the lock, for the throughput summary (D-112).
  struct Timed {
    double& total;
    Clock::time_point start = Clock::now();
    ~Timed() { total += std::chrono::duration<double>(Clock::now() - start).count(); }
  } timed{merge_seconds_};
  ++results_;
  decode_seconds_ += r.decode_seconds;
  auto lease = leases_.find(r.lease);
  if (lease == leases_.end()) {
    workers_[worker]->ackTile(r.lease, AckStatus::Rejected);  // cancelled or unknown lease
    return;
  }
  const WorkItem item = lease->second.item;
  // Per chunk, so items of several chunks (D-116) and single chunks share one scale for the late check.
  const double seconds = std::chrono::duration<double>(Clock::now() - lease->second.issued).count() /
                         scheduler_.chunksIn(lease->second.item);
  const LeaseInfo info = lease->second;
  leases_.erase(lease);

  const bool valid = item.id == r.item_id && item.rect == r.rect && item.sample_begin == r.sample_begin &&
                     item.sample_end == r.sample_end && r.mean.size() == r.rect.pixels();
  if (!valid) {
    spdlog::warn("rejected an invalid result for lease {} from worker {}; item requeued", r.lease, worker);
    workers_[worker]->ackTile(r.lease, AckStatus::Rejected);
    requeueLostLocked(r.lease, info);
    if (++rejects_[worker] == kMaxRejects)  // docs/GUIDE.md "Results": 3 rejected results get a worker kicked
      workers_[worker]->kick(fmt::format("{} invalid tile results (corrupt or not matching their lease)", kMaxRejects));
    serveLocked();
    return;
  }
  item_seconds_.push_back(seconds);
  if (item_seconds_.size() > kDurationWindow) item_seconds_.pop_front();
  device_last_result_[{worker, r.device}] = Clock::now();  // it is alive (D-111)
  std::deque<double>& mine = device_seconds_[{worker, r.device}];  // also when the result turns out a duplicate
  mine.push_back(seconds);
  if (mine.size() > kDeviceDurationWindow) mine.pop_front();

  // Already merged (a late copy of a re-issued item) or already waiting: the first valid result won.
  if (item.chunk < scheduler_.mergedChunks(item.tile, item.pass) || early_.count(item.id)) {
    workers_[worker]->ackTile(r.lease, AckStatus::Duplicate);
    checkFinishedLocked();
    return;
  }
  workers_[worker]->ackTile(r.lease, AckStatus::Accepted);
  if (info.duplicate) {
    ++duplicate_wins_;
    spdlog::info("tail: the copy of tile {} chunk {} on device {} of worker {} won", item.tile, item.chunk, r.device,
                 worker);
  }
  if (r.nan_count) {
    nan_count_ += r.nan_count;
    spdlog::warn("tile {} chunk {}: {} non-finite samples replaced by 0", item.tile, item.chunk, r.nan_count);
  }
  DeviceTally& t = tallies_[{worker, r.device}];
  ++t.items;
  t.samples += r.rect.pixels() * (r.sample_end - r.sample_begin);
  t.seconds += r.render_seconds;
  t.last_end = std::chrono::duration<double>(Clock::now() - start_).count();
  const double begin = std::max(0.0, t.last_end - r.render_seconds);
  if (t.first_begin < 0 || begin < t.first_begin) t.first_begin = begin;

  early_owner_[item.id] = worker;  // the live view colours each tile by the worker that merged it
  early_.emplace(item.id, std::move(r));
  mergeReadyLocked(item.tile, item.pass);
  checkFinishedLocked();
}

void RenderController::checkFinishedLocked() {
  if (finished_) return;
  // Stopped (time limit, Ctrl+C... ): finished once nothing is out and the AOVs are in (the denoiser needs them).
  if (scheduler_.done() || (scheduler_.stopped() && leases_.empty() && scheduler_.aovsDone())) {
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
    requeueLostLocked(id, it->second);
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
    requeueLostLocked(it->first, it->second);
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

double RenderController::medianSeconds(const std::deque<double>& window) {
  if (window.size() < 3) return -1;
  std::vector<double> v(window.begin(), window.end());
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

// A lease is late after 10x its device's median item time, so a slow device (a CPU next to fast GPUs) is not
// taken for a hung one; a device with fewer than 3 finished items is judged by the median of all devices (D-097).
// A device that has finished nothing yet gets at least kFirstItemsGrace: next to a fast GPU, a CPU's first items
// took longer than 10x the GPU's median and were all taken away (D-109).
// A device that keeps delivering is alive: its old leases wait in its queue, they are not lost. A lease is late only
// when its device has also delivered nothing for as long; reissuing a busy device's queue fed an overloaded
// controller more work and made every item look later still (net.scale_64, D-111).
void RenderController::reissueLateLocked() {
  if (paused_) return;  // nothing is late while nothing is handed out
  struct Timed {
    double& total;
    Clock::time_point start = Clock::now();
    ~Timed() { total += std::chrono::duration<double>(Clock::now() - start).count(); }
  } timed{late_seconds_};
  const double overall = medianSeconds(item_seconds_);
  const auto now = Clock::now();
  bool any = false;
  for (auto& [id, info] : leases_) {
    if (info.reissued) continue;
    const auto dev = device_seconds_.find({info.worker, info.device});
    const bool fresh = dev == device_seconds_.end() || dev->second.empty();
    double median = dev != device_seconds_.end() ? medianSeconds(dev->second) : -1;
    if (median < 0) median = overall;
    if (median < 0) continue;
    double limit = std::max(10 * median, kMinLateSeconds);
    if (fresh) limit = std::max(limit, std::chrono::duration<double>(scaledForTests(kFirstItemsGrace)).count());
    const double age = std::chrono::duration<double>(now - info.issued).count();
    if (age / scheduler_.chunksIn(info.item) <= limit) continue;  // item times are kept per chunk
    if (const auto last = device_last_result_.find({info.worker, info.device}); last != device_last_result_.end() &&
        std::chrono::duration<double>(now - last->second).count() <= limit)
      continue;
    spdlog::warn("lease {} (tile {} chunk {}) on device {} of worker {} is late ({:.1f}s, the limit is {:.1f}s{}); "
                 "handing it out again, the first valid result wins",
                 id, info.item.tile, info.item.chunk, info.device, info.worker, age, limit,
                 fresh ? " for a device's first items" : "");
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
  network_start_ = net::networkBusySeconds();
  tls_start_ = net::networkTlsSeconds();
  RenderOutcome outcome;
  std::vector<WorkerLink*> initial;
  {
    std::lock_guard lock(mutex_);
    if (options.live_view) liveInitLocked(options.live_view);
    if (scheduler_.done()) finished_ = true;  // resumed a film that was already complete
    running_ = true;
    initial = workerSnapshotLocked();
  }
  if (!finished_)
    for (WorkerLink* w : initial) w->offerScene(offer_);

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
      if (!wanted_.empty()) serveLocked();  // tail duplicates become worth it as leases age (D-091)
      const bool kill = killAfterItems() > 0 && merged_items_ >= killAfterItems();
      if (kill || std::chrono::duration<double>(now - lastCheckpoint).count() >= settings_.checkpoint_s) {
        lastCheckpoint = now;
        try {
          for (Film* f : films_)
            if (f) f->checkpoint();
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
        if (kill) {
          spdlog::warn("TEST HOOK: exiting with code 137 after {} merged chunks", merged_items_);
          spdlog::default_logger()->flush();
          std::_Exit(137);
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
  outcome.duplicated = duplicated_;
  outcome.duplicate_wins = duplicate_wins_;
  outcome.results = results_;
  outcome.decode_seconds = decode_seconds_;
  outcome.merge_seconds = merge_seconds_;
  outcome.serve_seconds = serve_seconds_;
  outcome.late_seconds = late_seconds_;
  outcome.tail_seconds = tail_seconds_;
  outcome.network_seconds = net::networkBusySeconds() - network_start_;
  outcome.tls_seconds = net::networkTlsSeconds() - tls_start_;
  outcome.progress = progressLocked();
  outcome.seconds = outcome.progress.elapsed_seconds;
  for (const auto& [key, tally] : tallies_) outcome.devices.push_back(tally);
  return outcome;
}

// ---------------------------------------------------------------- live view, pause, save now (UI)

void RenderController::liveInitLocked(uint32_t maxSide) {
  const FilmLayout& l = film_.layout();
  const double k = std::min(1.0, double(maxSide) / double(std::max(l.width, l.height)));
  live_w_ = std::max(1u, uint32_t(std::lround(l.width * k)));
  live_h_ = std::max(1u, uint32_t(std::lround(l.height * k)));
  live_sum_.assign(size_t(live_w_) * live_h_, Vec3f(0));
  live_weight_.assign(live_sum_.size(), 0.0);
  tile_owner_.assign(l.tileCount(), -1);
  // A resumed RAM film already holds samples; disk films show their tiles as they are merged again.
  if (!film_.inRam()) return;
  std::vector<Vec3f> mean;
  std::vector<uint32_t> counts;
  for (uint32_t t = 0; t < l.tileCount(); ++t) {
    if (!film_.tileSamples(t)) continue;
    const TileRect rect = l.tileRect(t);
    film_.readRect(rect, mean, &counts);
    for (uint32_t y = rect.y0; y < rect.y1; ++y)
      for (uint32_t x = rect.x0; x < rect.x1; ++x) {
        const size_t i = size_t(y - rect.y0) * rect.width() + (x - rect.x0);
        const size_t p = size_t(uint64_t(y) * live_h_ / l.height) * live_w_ + size_t(uint64_t(x) * live_w_ / l.width);
        live_sum_[p] += mean[i] * float(counts[i]);
        live_weight_[p] += counts[i];
      }
  }
  ++live_version_;
}

// Box filter weighted by samples: the downsampled running mean equals the downsampled film, whatever the order.
void RenderController::liveMergeLocked(const TileResult& r, WorkerId worker) {
  const FilmLayout& l = film_.layout();
  const float n = float(r.sample_end - r.sample_begin);
  for (uint32_t y = r.rect.y0; y < r.rect.y1; ++y) {
    const size_t row = size_t(uint64_t(y) * live_h_ / l.height) * live_w_;
    for (uint32_t x = r.rect.x0; x < r.rect.x1; ++x) {
      const size_t p = row + size_t(uint64_t(x) * live_w_ / l.width);
      live_sum_[p] += r.mean[size_t(y - r.rect.y0) * r.rect.width() + (x - r.rect.x0)] * n;
      live_weight_[p] += n;
    }
  }
  const uint32_t tile = WorkItem::idTile(r.item_id);
  if (tile < tile_owner_.size()) tile_owner_[tile] = int32_t(worker);
  ++live_version_;
}

bool RenderController::liveView(LiveView& out, uint64_t version) const {
  std::lock_guard lock(mutex_);
  if (!live_w_) return false;
  out.layout = film_.layout();
  out.tile_owner = tile_owner_;
  out.tile_busy.assign(out.layout.tileCount(), 0);
  for (const auto& [id, info] : leases_)
    if (info.item.tile < out.tile_busy.size()) out.tile_busy[info.item.tile] = 1;
  out.workers.clear();
  for (const auto& w : workers_) out.workers.push_back(w->info().name);
  if (live_version_ == version && out.version == version && !out.rgb.empty()) return true;  // pixels unchanged
  out.width = live_w_;
  out.height = live_h_;
  out.rgb.resize(live_sum_.size());
  for (size_t i = 0; i < live_sum_.size(); ++i)
    out.rgb[i] = live_weight_[i] > 0 ? live_sum_[i] * float(1.0 / live_weight_[i]) : Vec3f(0);
  out.version = live_version_;
  return true;
}

void RenderController::setPaused(bool paused) {
  std::vector<WorkerLink*> links;
  {
    std::lock_guard lock(mutex_);
    if (paused_ == paused || finished_) return;
    paused_ = paused;
    spdlog::info("render {}", paused ? "paused: no new work is handed out" : "resumed");
    links = workerSnapshotLocked();
    if (!paused) {
      // Leases out while paused were not late; restart their clocks so they are not reissued at once.
      const auto now = Clock::now();
      for (auto& [id, info] : leases_) info.issued = now;
      serveLocked();
    }
  }
  for (WorkerLink* w : links) w->pause(paused);
}

void RenderController::readFilm(const TileRect& rect, std::vector<Vec3f>& out) const {
  std::lock_guard lock(mutex_);
  film_.readRect(rect, out);
}

bool RenderController::paused() const {
  std::lock_guard lock(mutex_);
  return paused_;
}

bool RenderController::writeImageNow(const RenderSettings& settings, std::string& error) {
  std::lock_guard lock(mutex_);  // no merge while the image is written
  try {
    std::vector<std::string> notes;
    writeOutput(film_, settings, {{"Software", "Penumbra"}, {"Incomplete", "true"}}, notes);
    for (const auto& n : notes) spdlog::info("{}", n);
    spdlog::info("saved the current image to {}", pathToUtf8(settings.output));
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

}  // namespace pen
