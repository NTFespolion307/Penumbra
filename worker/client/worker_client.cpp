#include "worker/client/worker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "core/exit_codes.h"
#include "core/scene/scene_loader.h"
#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/settings/serialize.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"
#include "net/connection.h"
#include "net/join_code.h"
#include "net/tile_codec.h"
#include "worker/device_manager.h"
#include "worker/device_prep.h"
#include "worker/device_slot.h"
#include "worker/scene_cache.h"

namespace pen {

namespace {

namespace wire = net::wire;
using net::MsgType;
using Clock = std::chrono::steady_clock;

constexpr uint64_t kUploadBoundBytes = 512ull << 20;  // docs/GUIDE.md "Uploader and backpressure"
constexpr auto kConnectRetry = std::chrono::seconds(15);
constexpr auto kStatsPeriod = std::chrono::seconds(2);

uint64_t nowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

// The worker's own settings that stay local: which devices render and how (docs/GUIDE.md "--devices: which
// local devices render (controller or worker)"). Everything else comes from the controller.
void keepLocalDeviceSettings(RenderSettings& remote, const RenderSettings& local) {
  remote.backend = local.backend;
  remote.devices = local.devices;
  remote.threads = local.threads;
  remote.vram_limit = local.vram_limit;
  remote.ram_limit = local.ram_limit;
  remote.no_texture_downsize = local.no_texture_downsize;
}

class Worker {
 public:
  explicit Worker(const WorkerOptions& o)
      : opt_(o),
        local_(o.settings),
        cache_(o.settings.cache_dir.empty() ? paths::sceneCacheDir() : o.settings.cache_dir, o.settings.cache_size) {}
  ~Worker() {
    stopSlots(true);
    io_.stop();  // no network callbacks after this
  }

  int run();

 private:
  struct Inbox {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<net::Message> messages;
    bool closed = false;
    std::string reason;
  };
  struct DeviceCounters {
    uint64_t tiles = 0, samples = 0;
    double last_busy = 0;
    uint64_t last_tiles = 0, last_samples = 0;
  };

  int connect();
  // Returns an exit code when the worker should stop, -1 to continue.
  int handle(net::Message& msg);
  void onSceneOffer(const wire::SceneOfferT& m);
  void onSceneChunk(const wire::SceneChunkT& m);
  void finishDownload();
  void prepare(const std::filesystem::path& folder);
  void onLease(const wire::LeaseT& m);
  void rejectScene(uint64_t renderId, const std::string& reason);
  void startSlots();
  void stopSlots(bool cancel);
  void flushRequests();
  void sendStats();
  bool uploadBound() const { return conn_ && conn_->queuedBytes() > kUploadBoundBytes; }
  int leave(const std::string& reason, int exitCode);

  WorkerOptions opt_;
  RenderSettings local_;
  std::string name_;
  std::vector<std::unique_ptr<IBackend>> devices_;

  net::IoThread io_;
  std::shared_ptr<net::Connection> conn_;
  Inbox inbox_;
  bool welcomed_ = false;
  bool completed_ = false;  // RenderComplete received and Bye sent

  // The current render.
  uint64_t render_id_ = 0;
  uint64_t settings_hash_ = 0;
  RenderSettings render_;  // as the controller sent them (controller paths)
  std::shared_ptr<Scene> scene_;
  SceneCache cache_;
  std::shared_ptr<const ScenePackage> package_;
  uint64_t package_hash_ = 0;
  Clock::time_point download_started_;
  double next_progress_ = 0.25;
  std::vector<std::unique_ptr<DeviceSlot>> slots_;

  // Shared with the slot threads.
  std::mutex mutex_;
  std::map<uint32_t, int> pending_requests_;  // device -> items wanted, not sent yet (paused or upload-bound)
  std::set<LeaseId> outstanding_;
  std::vector<bool> failed_;
  std::map<uint32_t, DeviceCounters> counters_;
  std::atomic<bool> paused_{false};

  CpuUsage cpu_;
  Clock::time_point last_stats_ = Clock::now();
  bool was_upload_bound_ = false;
};

int Worker::connect() {
  net::JoinAddress addr;
  try {
    addr = net::parseJoinAddress(opt_.join);
  } catch (const std::invalid_argument& e) {
    spdlog::error("--join {}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  const auto code = net::normalizeJoinCode(opt_.code);
  if (!code) {
    spdlog::error("--code '{}' is not a join code (16 characters like K7QD-M2XR-9FHT-4WNB)", opt_.code);
    return toInt(ExitCode::BadArguments);
  }
  const net::JoinKey key = net::deriveJoinKey(*code);
  spdlog::info("connecting to {}:{}", addr.host, addr.port);
  const auto deadline = Clock::now() + kConnectRetry;
  for (;;) {
    try {
      conn_ = net::connectTo(io_, addr.host, addr.port, key);
      break;
    } catch (const net::ConnectError& e) {
      // The controller may still be starting: keep trying for a while when nothing listens yet.
      if (!e.refused || Clock::now() >= deadline) {
        spdlog::error("{}. Check the address, that the controller is hosting (--host), and the firewall "
                      "(TCP port {}).",
                      e.what(), addr.port);
        return toInt(ExitCode::ConnectionLost);
      }
      if (opt_.interrupt && opt_.interrupt->load()) return toInt(ExitCode::Interrupted);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
  conn_->start({
      [this](net::Message&& msg, uint16_t version) {
        if (version != kProtocolVersion && msg.type != MsgType::Reject) {
          conn_->close(fmt::format("the controller speaks protocol version {}, this worker {}", version,
                                   kProtocolVersion));
          return;
        }
        std::lock_guard lock(inbox_.mutex);
        inbox_.messages.push_back(std::move(msg));
        inbox_.cv.notify_one();
      },
      [this](const std::string& reason) {
        std::lock_guard lock(inbox_.mutex);
        inbox_.closed = true;
        inbox_.reason = reason;
        inbox_.cv.notify_one();
      },
  });

  const SystemInfo sys = querySystem();
  wire::HelloT hello;
  hello.protocol_version = kProtocolVersion;
  hello.app_version = std::string(appVersion());
  hello.git_hash = std::string(gitHash());
  if (const char* fake = std::getenv("PENUMBRA_TEST_GIT_HASH")) {  // test-only (D-047): pose as another build
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_GIT_HASH): claiming git hash {}", fake);
    hello.git_hash = fake;
  }
  hello.worker_name = name_;
  hello.os = sys.os;
  hello.cpu = sys.cpu;
  hello.logical_cores = sys.logical_cores;
  hello.ram_total = sys.ram_total;
  for (const auto& d : devices_) {
    const DeviceInfo i = d->info();
    auto desc = std::make_unique<wire::DeviceDescT>();
    desc->name = i.name;
    desc->backend = static_cast<uint8_t>(i.backend);
    desc->threads = i.threads;
    desc->memory_bytes = i.memory_bytes;
    desc->hardware_rt = i.hardware_rt;
    hello.devices.push_back(std::move(desc));
  }
  conn_->send(hello);
  return -1;
}

int Worker::run() {
  name_ = local_.worker_name.empty() ? hostName() : local_.worker_name;
  try {
    DeviceSelection sel = createDevices(local_, /*controllerHost=*/false);
    for (const auto& n : sel.notes) spdlog::info("{}", n);
    devices_ = std::move(sel.devices);
  } catch (const NoDeviceError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  } catch (const BackendError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  }
  for (size_t i = 0; i < devices_.size(); ++i) {
    const DeviceInfo d = devices_[i]->info();
    spdlog::info("device {}: {} (backend {}, {} threads)", i, d.name, toString(d.backend), d.threads);
  }
  if (const int rc = connect(); rc >= 0) return rc;

  for (;;) {
    std::deque<net::Message> batch;
    bool closed = false;
    std::string reason;
    {
      std::unique_lock lock(inbox_.mutex);
      inbox_.cv.wait_for(lock, std::chrono::milliseconds(100),
                         [&] { return !inbox_.messages.empty() || inbox_.closed; });
      batch.swap(inbox_.messages);
      closed = inbox_.closed;
      reason = inbox_.reason;
    }
    for (auto& msg : batch)
      if (const int rc = handle(msg); rc >= 0) return rc;
    if (closed) {
      stopSlots(true);
      if (completed_) return toInt(ExitCode::Success);
      if (!welcomed_ && conn_->handshakeFailed()) {
        spdlog::debug("handshake: {}", reason);
        spdlog::error("could not join: the controller did not accept the join code. Check --code (the controller "
                      "prints it when it starts hosting); after more than 5 wrong codes in a minute the controller "
                      "ignores this address for 10 minutes.");
        return toInt(ExitCode::RejectedByController);
      }
      spdlog::error("lost the connection to the controller: {}", reason);
      return toInt(ExitCode::ConnectionLost);
    }
    if (opt_.interrupt && opt_.interrupt->load()) {
      spdlog::warn("interrupted: handing the unfinished leases back to the controller");
      return leave("the worker was stopped (Ctrl+C)", toInt(ExitCode::Interrupted));
    }
    if (!slots_.empty()) {
      bool allFailed = true;
      {
        std::lock_guard lock(mutex_);
        for (bool f : failed_) allFailed = allFailed && f;
      }
      if (allFailed) {
        spdlog::error("every device failed; leaving the render");
        return leave("every device of this worker failed", toInt(ExitCode::NoUsableDevice));
      }
    }
    flushRequests();
    if (welcomed_ && Clock::now() - last_stats_ >= kStatsPeriod) sendStats();
  }
}

int Worker::handle(net::Message& msg) {
  try {
    if (!welcomed_ && msg.type != MsgType::Welcome && msg.type != MsgType::Reject) {
      spdlog::error("the controller sent {} before Welcome", net::toString(msg.type));
      return toInt(ExitCode::ConnectionLost);
    }
    switch (msg.type) {
      case MsgType::Welcome: {
        const auto m = net::decode<wire::WelcomeT>(msg.payload);
        welcomed_ = true;
        spdlog::info("joined the controller as worker {} (\"{}\"); waiting for a render", m.worker_id, name_);
        break;
      }
      case MsgType::Reject: {
        const auto m = net::decode<wire::RejectT>(msg.payload);
        spdlog::error("the controller rejected this worker: {}", m.text);
        return toInt(ExitCode::RejectedByController);
      }
      case MsgType::Kick: {
        const auto m = net::decode<wire::KickT>(msg.payload);
        spdlog::error("the controller removed this worker: {}", m.reason);
        stopSlots(true);
        return toInt(ExitCode::RejectedByController);
      }
      case MsgType::SceneOffer: onSceneOffer(net::decode<wire::SceneOfferT>(msg.payload)); break;
      case MsgType::SceneChunk: onSceneChunk(net::decode<wire::SceneChunkT>(msg.payload)); break;
      case MsgType::Lease: onLease(net::decode<wire::LeaseT>(msg.payload)); break;
      case MsgType::Cancel: {
        net::decode<wire::CancelT>(msg.payload);  // validates; the controller only cancels everything so far
        for (auto& s : slots_) s->clearQueue();
        for (auto& d : devices_) d->cancel();
        std::lock_guard lock(mutex_);
        outstanding_.clear();
        break;
      }
      case MsgType::TileAck: {
        const auto m = net::decode<wire::TileAckT>(msg.payload);
        if (m.status == wire::AckStatus::Rejected)
          spdlog::warn("the controller rejected the tile for lease {}{}", m.lease_id,
                       m.reason.empty() ? "" : ": " + m.reason);
        break;
      }
      case MsgType::Pause: {
        const auto m = net::decode<wire::PauseT>(msg.payload);
        paused_ = m.paused;
        spdlog::info("{}", m.paused ? "paused by the controller" : "resumed by the controller");
        break;
      }
      case MsgType::RenderComplete: {
        stopSlots(true);
        scene_.reset();
        spdlog::info("render complete");
        return leave("render complete", -2);
      }
      case MsgType::Heartbeat: {
        const auto m = net::decode<wire::HeartbeatT>(msg.payload);
        wire::HeartbeatT reply;
        reply.sent_us = nowMicros();
        reply.echo_us = m.sent_us;
        conn_->send(reply);
        break;
      }
      default:
        spdlog::warn("ignoring an unexpected {} message from the controller", net::toString(msg.type));
        break;
    }
  } catch (const net::ProtocolError& e) {
    spdlog::error("bad {} message from the controller: {}", net::toString(msg.type), e.what());
    return toInt(ExitCode::ConnectionLost);
  }
  return -1;
}

// Says Bye and closes. exitCode -2: the render completed; wait for the controller to close, then exit 0.
int Worker::leave(const std::string& reason, int exitCode) {
  std::vector<LeaseId> lost;
  {
    std::lock_guard lock(mutex_);
    lost.assign(outstanding_.begin(), outstanding_.end());
    outstanding_.clear();
  }
  if (!lost.empty()) {
    wire::ReleaseT r;
    r.leases = lost;
    conn_->send(r);
  }
  wire::ByeT bye;
  bye.reason = reason;
  conn_->send(bye);
  conn_->closeAfterSend(reason);
  if (exitCode == -2) {
    completed_ = true;
    return -1;  // the main loop exits with 0 once the connection has closed
  }
  stopSlots(true);
  // Give the frames a moment to leave.
  std::unique_lock lock(inbox_.mutex);
  inbox_.cv.wait_for(lock, std::chrono::seconds(3), [&] { return inbox_.closed; });
  return exitCode;
}

void Worker::rejectScene(uint64_t renderId, const std::string& reason) {
  spdlog::error("cannot take this render: {}", reason);
  wire::WorkerReadyT r;
  r.render_id = renderId;
  r.scene_rejected = true;
  r.reason = reason;
  conn_->send(r);
}

void Worker::onSceneOffer(const wire::SceneOfferT& m) {
  stopSlots(true);
  scene_.reset();
  package_.reset();
  render_id_ = m.render_id;
  RenderSettings s;
  try {
    s = settingsFromJson(nlohmann::json::parse(m.settings_json));
  } catch (const std::exception& e) {
    return rejectScene(m.render_id, fmt::format("the render settings are invalid: {}", e.what()));
  }
  if (pixelSettingsHash(s) != m.settings_hash)
    return rejectScene(m.render_id, "the render settings changed in transfer (settings hash mismatch)");
  ScenePackage pkg;
  try {
    pkg = ScenePackage::fromManifest(m.manifest);
  } catch (const SceneError& e) {
    return rejectScene(m.render_id, e.what());
  }
  if (pkg.hash() != m.package_hash) return rejectScene(m.render_id, "the scene manifest changed in transfer");
  render_ = s;
  settings_hash_ = m.settings_hash;
  package_hash_ = m.package_hash;
  spdlog::info("render offered: {}x{}, {} spp; scene {} ({} files, {})", m.film_width, m.film_height, s.spp,
               pkg.entry, pkg.files.size(), formatBytes(pkg.total_bytes));

  wire::SceneHaveT have;
  have.render_id = m.render_id;
  if (auto folder = cache_.find(pkg, m.package_hash)) {
    spdlog::info("the scene is in the cache: {}", pathToUtf8(*folder));
    have.status = wire::SceneStatus::Cached;
    conn_->send(have);
    package_ = std::make_shared<ScenePackage>(std::move(pkg));
    return prepare(*folder);
  }
  if (auto why = cache_.cannotHold(pkg)) {
    spdlog::error("cannot take this render: {}", *why);
    have.status = wire::SceneStatus::Rejected;
    have.reason = *why;
    conn_->send(have);
    return;
  }
  uint32_t first = 0;
  try {
    first = cache_.beginDownload(pkg, m.package_hash);
  } catch (const FileError& e) {
    have.status = wire::SceneStatus::Rejected;
    have.reason = fmt::format("cannot write to the scene cache: {}", e.what());
    spdlog::error("cannot take this render: {}", have.reason);
    conn_->send(have);
    return;
  }
  package_ = std::make_shared<ScenePackage>(std::move(pkg));
  download_started_ = Clock::now();
  next_progress_ = 0.25;
  if (first == package_->chunkCount()) {  // everything was already downloaded before an interruption
    have.status = wire::SceneStatus::Cached;
    conn_->send(have);
    return finishDownload();
  }
  if (first > 0)
    spdlog::info("downloading the scene: resuming at chunk {} of {}", first, package_->chunkCount());
  else
    spdlog::info("downloading the scene: {} in {} chunks", formatBytes(package_->total_bytes), package_->chunkCount());
  have.status = wire::SceneStatus::Need;
  have.first_missing = first;
  conn_->send(have);
}

void Worker::onSceneChunk(const wire::SceneChunkT& m) {
  if (!package_ || m.package_hash != package_hash_ || cache_.downloadComplete()) return;  // for an older offer
  try {
    const std::vector<uint8_t> raw = net::decompress(m.data, m.raw_size);
    cache_.addChunk(m.index, raw);
  } catch (const std::exception& e) {
    return rejectScene(render_id_, fmt::format("the scene download failed: {}", e.what()));
  }
  // Test-only (D-066): crash right after this many chunks, to test resumed downloads without timing races.
  if (const char* n = std::getenv("PENUMBRA_TEST_EXIT_AFTER_CHUNKS"); n && m.index + 1 == std::strtoul(n, nullptr, 10)) {
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_EXIT_AFTER_CHUNKS): exiting abruptly after chunk {}", m.index);
    spdlog::default_logger()->flush();
    std::_Exit(137);
  }
  const double done = double(m.index + 1) / package_->chunkCount();
  if (done >= next_progress_ && !cache_.downloadComplete()) {
    spdlog::info("downloading the scene: {:.0f}%", done * 100);
    while (next_progress_ <= done) next_progress_ += 0.25;
  }
  if (cache_.downloadComplete()) finishDownload();
}

void Worker::finishDownload() {
  std::filesystem::path folder;
  try {
    folder = cache_.finishDownload();
  } catch (const std::exception& e) {
    return rejectScene(render_id_, fmt::format("the scene download failed: {}", e.what()));
  }
  spdlog::info("scene downloaded in {:.1f}s and verified", std::chrono::duration<double>(Clock::now() - download_started_).count());
  prepare(folder);
}

// Loads the scene from its cached package and builds every device (docs/GUIDE.md "Ready before work").
void Worker::prepare(const std::filesystem::path& folder) {
  RenderSettings s = package_->relocate(render_, folder);
  keepLocalDeviceSettings(s, local_);
  try {
    const auto t0 = Clock::now();
    std::vector<std::string> warnings;
    scene_ = std::make_shared<Scene>(loadScene(s, warnings));
    for (const auto& w : warnings) spdlog::warn("scene: {}", w);
    spdlog::info("scene loaded in {:.2f}s", std::chrono::duration<double>(Clock::now() - t0).count());
  } catch (const SceneError& e) {
    return rejectScene(render_id_, e.what());
  }

  wire::WorkerReadyT ready;
  ready.render_id = render_id_;
  try {
    for (const PreparedDevice& p : prepareDevices(*scene_, s, devices_)) {
      spdlog::info("device {} ready in {:.2f}s: {}", p.device, p.build_seconds, p.summary);
      auto d = std::make_unique<wire::DeviceReadyDescT>();
      d->device = p.device;
      d->backend = static_cast<uint8_t>(p.backend);
      d->memory_tier = p.memory_tier;
      d->build_seconds = p.build_seconds;
      d->summary = p.summary;
      ready.devices.push_back(std::move(d));
    }
  } catch (const std::exception& e) {
    return rejectScene(render_id_, e.what());
  }
  conn_->send(ready);
  startSlots();
}

void Worker::startSlots() {
  {
    std::lock_guard lock(mutex_);
    failed_.assign(devices_.size(), false);
    pending_requests_.clear();
    outstanding_.clear();
    counters_.clear();
  }
  for (uint32_t d = 0; d < devices_.size(); ++d) {
    DeviceSlot::Callbacks cb;
    cb.request_work = [this](uint32_t device, int count) {
      {
        std::lock_guard lock(mutex_);
        pending_requests_[device] = count;
      }
      flushRequests();
    };
    cb.result = [this](TileResult&& r) {
      wire::TileResultT m;
      m.lease_id = r.lease;
      m.device = r.device;
      m.item_id = r.item_id;
      m.x0 = r.rect.x0;
      m.y0 = r.rect.y0;
      m.x1 = r.rect.x1;
      m.y1 = r.rect.y1;
      m.sample_begin = r.sample_begin;
      m.sample_end = r.sample_end;
      m.encoding = wire::TileEncoding::Float32Shuffled;
      m.pixels = net::encodeTilePixels(r.mean);
      m.checksum = net::tileChecksum(r.mean);
      m.nan_count = r.nan_count;
      m.render_seconds = r.render_seconds;
      {
        std::lock_guard lock(mutex_);
        if (!outstanding_.erase(r.lease)) return;  // cancelled meanwhile
        DeviceCounters& c = counters_[r.device];
        ++c.tiles;
        c.samples += r.rect.pixels() * (r.sample_end - r.sample_begin);
      }
      conn_->send(m, net::Priority::Bulk);
    };
    cb.failed = [this](uint32_t device, const std::string& error, const std::vector<LeaseId>& lost) {
      spdlog::error("device {} failed and is removed: {}", device, error);
      {
        std::lock_guard lock(mutex_);
        failed_[device] = true;
        pending_requests_.erase(device);
        for (LeaseId l : lost) outstanding_.erase(l);
      }
      wire::DeviceFailedT f;
      f.device = device;
      f.error = error;
      conn_->send(f);
      wire::ReleaseT r;
      r.leases = lost;
      conn_->send(r);
    };
    slots_.push_back(std::make_unique<DeviceSlot>(d, *devices_[d], cb));
  }
  for (auto& s : slots_) s->start();
}

void Worker::stopSlots(bool cancel) {
  if (slots_.empty()) return;
  if (cancel) {
    for (auto& s : slots_) s->clearQueue();
    for (auto& d : devices_) d->cancel();
  }
  for (auto& s : slots_) s->finish();
  for (auto& s : slots_) s->join();
  slots_.clear();
}

void Worker::onLease(const wire::LeaseT& m) {
  const auto release = [&](const std::string& why) {
    spdlog::warn("handing back lease {}: {}", m.lease_id, why);
    wire::ReleaseT r;
    r.leases = {m.lease_id};
    conn_->send(r);
  };
  if (m.settings_hash != settings_hash_) return release("it is for other render settings");
  if (m.device >= slots_.size()) return release(fmt::format("there is no device {}", m.device));
  bool deviceFailed = false;
  {
    std::lock_guard lock(mutex_);
    deviceFailed = failed_[m.device];
    if (!deviceFailed) outstanding_.insert(m.lease_id);
  }
  if (deviceFailed) return release(fmt::format("device {} has failed", m.device));
  WorkItem item;
  item.id = m.item_id;
  item.tile = static_cast<uint32_t>(m.item_id >> 32);
  item.chunk = static_cast<uint32_t>(m.item_id & 0xffffffffu);
  item.rect = {m.x0, m.y0, m.x1, m.y1};
  item.sample_begin = m.sample_begin;
  item.sample_end = m.sample_end;
  item.lease = m.lease_id;
  slots_[m.device]->addLease(item);
}

void Worker::flushRequests() {
  if (paused_ || completed_ || !conn_) return;
  const bool bound = uploadBound();
  if (bound != was_upload_bound_) {
    was_upload_bound_ = bound;
    if (bound) spdlog::warn("upload-bound: more than 512 MB of tiles wait to be sent; not asking for new work");
    else spdlog::info("upload caught up; asking for work again");
  }
  if (bound) return;
  std::map<uint32_t, int> requests;
  {
    std::lock_guard lock(mutex_);
    requests.swap(pending_requests_);
  }
  for (const auto& [device, count] : requests) {
    wire::RequestWorkT r;
    r.device = device;
    r.count = count;
    conn_->send(r);
  }
}

void Worker::sendStats() {
  const double dt = std::chrono::duration<double>(Clock::now() - last_stats_).count();
  last_stats_ = Clock::now();
  const SystemInfo sys = querySystem();
  wire::StatsT s;
  s.cpu_percent = static_cast<float>(cpu_.sample());
  s.ram_total = sys.ram_total;
  s.ram_used = sys.ram_total - sys.ram_available;
  s.upload_bound = uploadBound();
  std::lock_guard lock(mutex_);
  for (uint32_t d = 0; d < slots_.size(); ++d) {
    DeviceCounters& c = counters_[d];
    const double busy = slots_[d]->busySeconds();
    auto ds = std::make_unique<wire::DeviceStatsT>();
    ds->device = d;
    ds->busy_percent = dt > 0 ? static_cast<float>(std::min(100.0, 100.0 * (busy - c.last_busy) / dt)) : 0.f;
    ds->temperature_c = -300.f;
    ds->tiles_per_s = dt > 0 ? static_cast<float>((c.tiles - c.last_tiles) / dt) : 0.f;
    ds->samples_per_s = dt > 0 ? double(c.samples - c.last_samples) / dt : 0.0;
    c.last_busy = busy;
    c.last_tiles = c.tiles;
    c.last_samples = c.samples;
    s.devices.push_back(std::move(ds));
  }
  conn_->send(s);
}

}  // namespace

int runWorker(const WorkerOptions& options) {
  Worker w(options);
  return w.run();
}

}  // namespace pen
