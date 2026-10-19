#include "controller/hosting/scene_uploader.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "core/settings/parse.h"
#include "core/system/file.h"
#include "net/tile_codec.h"

namespace pen {

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t kMaxQueued = 2ull * ScenePackage::kChunkBytes;
}  // namespace

SceneUploader::SceneUploader(uint64_t limitBytesPerSecond) : limit_(limitBytesPerSecond) {
  thread_ = std::thread([this] { run(); });
}

SceneUploader::~SceneUploader() { stop(); }

void SceneUploader::stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void SceneUploader::start(std::shared_ptr<net::Connection> connection, std::shared_ptr<const ScenePackage> pkg,
                          uint64_t hash, uint32_t first, const std::string& workerName) {
  std::lock_guard lock(mutex_);
  std::erase_if(transfers_, [&](const Transfer& t) { return t.connection == connection; });
  transfers_.push_back({std::move(connection), std::move(pkg), hash, first, workerName, 0, Clock::now()});
  cv_.notify_all();
}

void SceneUploader::run() {
  // Token bucket for --upload-limit: at most one second of budget saved up.
  double tokens = 0;
  auto last = Clock::now();
  size_t turn = 0;
  std::unique_lock lock(mutex_);
  while (!stop_) {
    std::erase_if(transfers_, [](const Transfer& t) { return !t.connection->isOpen(); });
    if (transfers_.empty()) {
      cv_.wait(lock, [&] { return stop_ || !transfers_.empty(); });
      last = Clock::now();
      continue;
    }
    const auto now = Clock::now();
    if (limit_ > 0) {
      tokens = std::min<double>(double(limit_), tokens + std::chrono::duration<double>(now - last).count() * double(limit_));
      last = now;
    }
    // The next transfer, round-robin, whose connection has room.
    Transfer* t = nullptr;
    for (size_t i = 0; i < transfers_.size(); ++i) {
      Transfer& c = transfers_[(turn + i) % transfers_.size()];
      if (c.connection->queuedBytes() < kMaxQueued) {
        t = &c;
        turn = (turn + i + 1) % transfers_.size();
        break;
      }
    }
    if (!t || (limit_ > 0 && tokens < 0)) {  // a chunk may overdraw; the debt is waited off before the next
      cv_.wait_for(lock, std::chrono::milliseconds(5));
      continue;
    }
    Transfer job = *t;
    lock.unlock();
    std::vector<uint8_t> raw;
    try {
      raw = readPackageChunk(*job.pkg, job.next);
    } catch (const FileError& e) {
      spdlog::error("cannot send the scene to '{}': {}", job.name, e.what());
      job.connection->close("the scene could not be read");
      lock.lock();
      continue;
    }
    net::wire::SceneChunkT m;
    m.package_hash = job.hash;
    m.index = job.next;
    m.raw_size = static_cast<uint32_t>(raw.size());
    m.data = net::compress(raw);
    const size_t wire = m.data.size();
    job.connection->send(m, net::Priority::Bulk);
    lock.lock();
    if (limit_ > 0) tokens -= double(wire);  // the limit counts bytes on the wire
    auto it = std::find_if(transfers_.begin(), transfers_.end(),
                           [&](const Transfer& x) { return x.connection == job.connection && x.hash == job.hash; });
    if (it == transfers_.end()) continue;  // replaced or stopped meanwhile
    it->bytes_sent += wire;
    if (++it->next >= it->pkg->chunkCount()) {
      const double s = std::chrono::duration<double>(Clock::now() - it->started).count();
      spdlog::info("scene sent to '{}': {} on the wire in {:.1f}s", it->name, formatBytes(it->bytes_sent), s);
      transfers_.erase(it);
    }
  }
}

}  // namespace pen
