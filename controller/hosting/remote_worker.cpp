#include "controller/hosting/remote_worker.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <random>

#include "core/film/session.h"
#include "core/settings/parse.h"
#include "core/settings/serialize.h"
#include "net/tile_codec.h"

namespace pen {

namespace wire = net::wire;

RemoteWorker::RemoteWorker(WorkerId id, WorkerEvents& events, SceneUploader& uploader,
                           std::shared_ptr<net::Connection> connection, wire::HelloT hello, uint64_t sessionId)
    : id_(id), events_(events), uploader_(uploader), connection_(std::move(connection)), hello_(std::move(hello)) {
  info_.name = hello_.worker_name;
  info_.local = false;
  info_.os = hello_.os;
  info_.address = connection_->peer();
  info_.logical_cores = hello_.logical_cores;
  info_.ram_total = hello_.ram_total;
  for (const auto& d : hello_.devices) {
    DeviceInfo di;
    di.name = d->name;
    di.backend = static_cast<BackendKind>(d->backend);
    di.threads = d->threads;
    di.memory_bytes = d->memory_bytes;
    di.hardware_rt = d->hardware_rt;
    info_.devices.push_back(di);
  }
  wire::WelcomeT welcome;
  welcome.session_id = sessionId;
  welcome.worker_id = id;
  welcome.controller_name = "penumbra";
  welcome.heartbeat_ms = 5000;
  connection_->send(welcome);
}

RemoteWorker::~RemoteWorker() = default;

void RemoteWorker::offerScene(const RenderOffer& offer) {
  // The settings travel as the controller has them; the worker points the scene paths into its cached copy of
  // the package (ScenePackage::relocate).
  const RenderSettings& settings = offer.settings;
  wire::SceneOfferT m;
  m.render_id = std::random_device{}() | (uint64_t(std::random_device{}()) << 32);
  m.settings_json = settingsToJson(settings).dump();
  m.settings_hash = pixelSettingsHash(settings);
  m.scene_hash = sceneFingerprint(settings);
  m.film_width = settings.outputWidth();
  m.film_height = settings.outputHeight();
  m.manifest = offer.package->manifest();
  m.package_hash = offer.package->hash();
  {
    std::lock_guard lock(mutex_);
    render_id_ = m.render_id;
    settings_hash_ = m.settings_hash;
    package_ = offer.package;
    package_hash_ = m.package_hash;
  }
  connection_->send(m);
}

void RemoteWorker::sendLease(const Lease& lease) {
  wire::LeaseT m;
  m.lease_id = lease.id;
  m.device = lease.device;
  m.item_id = lease.item.id;
  m.x0 = lease.item.rect.x0;
  m.y0 = lease.item.rect.y0;
  m.x1 = lease.item.rect.x1;
  m.y1 = lease.item.rect.y1;
  m.sample_begin = lease.item.sample_begin;
  m.sample_end = lease.item.sample_end;
  m.settings_hash = settings_hash_;
  connection_->send(m);
}

void RemoteWorker::cancel(std::span<const LeaseId> leases) {
  wire::CancelT m;
  m.all = leases.empty();
  m.leases.assign(leases.begin(), leases.end());
  connection_->send(m);
}

void RemoteWorker::ackTile(LeaseId lease, AckStatus status) {
  wire::TileAckT m;
  m.lease_id = lease;
  m.status = static_cast<wire::AckStatus>(status);
  connection_->send(m);
}

void RemoteWorker::pause(bool paused) {
  wire::PauseT m;
  m.paused = paused;
  connection_->send(m);
}

void RemoteWorker::complete() {
  wire::RenderCompleteT m;
  m.render_id = render_id_;
  connection_->send(m);
}

void RemoteWorker::disconnect(const std::string& reason) { connection_->close(reason); }

void RemoteWorker::kick(const std::string& reason) {
  spdlog::warn("kicking {}: {}", info_.name, reason);
  wire::KickT m;
  m.reason = reason;
  connection_->send(m);
  connection_->closeAfterSend(fmt::format("kicked: {}", reason));
}

TileResult RemoteWorker::decodeResult(const wire::TileResultT& m) const {
  TileResult r;
  r.item_id = m.item_id;
  r.lease = m.lease_id;
  r.device = m.device;
  r.rect = {m.x0, m.y0, m.x1, m.y1};
  r.sample_begin = m.sample_begin;
  r.sample_end = m.sample_end;
  r.nan_count = m.nan_count;
  r.render_seconds = m.render_seconds;
  if (m.x1 <= m.x0 || m.y1 <= m.y0 || m.encoding != wire::TileEncoding::Float32Shuffled) return r;
  try {
    std::vector<Vec3f> mean = net::decodeTilePixels(m.pixels, r.rect.pixels());
    if (net::tileChecksum(mean) == m.checksum) r.mean = std::move(mean);
    else spdlog::warn("{}: tile for lease {} fails its checksum", info_.name, m.lease_id);
  } catch (const net::ProtocolError& e) {
    spdlog::warn("{}: tile for lease {} cannot be decoded: {}", info_.name, m.lease_id, e.what());
  }
  return r;  // an empty mean fails the controller's validity check: rejected and requeued
}

void RemoteWorker::onMessage(net::Message&& msg) {
  using net::MsgType;
  try {
    switch (msg.type) {
      case MsgType::SceneHave: {
        const auto m = net::decode<wire::SceneHaveT>(msg.payload);
        if (m.render_id != render_id_) return;
        if (m.status == wire::SceneStatus::Cached) {
          spdlog::info("{} has the scene in its cache", info_.name);
        } else if (m.status == wire::SceneStatus::Rejected) {
          spdlog::warn("{} cannot take this scene: {}; the render continues without it", info_.name, m.reason);
        } else if (m.first_missing < package_->chunkCount()) {
          const uint64_t left = package_->total_bytes - uint64_t(m.first_missing) * ScenePackage::kChunkBytes;
          spdlog::info("sending the scene to {}: {}{}", info_.name, formatBytes(left),
                       m.first_missing ? fmt::format(" (resuming at chunk {} of {})", m.first_missing,
                                                     package_->chunkCount())
                                       : std::string());
          uploader_.start(connection_, package_, package_hash_, m.first_missing, info_.name);
        }
        break;
      }
      case MsgType::WorkerReady: {
        const auto m = net::decode<wire::WorkerReadyT>(msg.payload);
        if (m.render_id != render_id_) return;  // for an earlier render
        if (m.scene_rejected) {
          spdlog::warn("{} cannot take this scene: {}", info_.name, m.reason);
          return;
        }
        std::vector<DeviceReady> ready;
        for (const auto& d : m.devices)
          ready.push_back({d->device, static_cast<BackendKind>(d->backend), d->memory_tier, d->build_seconds, d->summary,
                           d->skipped, d->reason});
        events_.onReady(id_, ready);
        break;
      }
      case MsgType::RequestWork: {
        const auto m = net::decode<wire::RequestWorkT>(msg.payload);
        events_.onRequestWork(id_, m.device, m.count);
        break;
      }
      case MsgType::TileResult: {
        events_.onTileResult(id_, decodeResult(net::decode<wire::TileResultT>(msg.payload)));
        break;
      }
      case MsgType::Release: {
        const auto m = net::decode<wire::ReleaseT>(msg.payload);
        std::vector<LeaseId> leases(m.leases.begin(), m.leases.end());
        events_.onRelease(id_, leases);
        break;
      }
      case MsgType::DeviceFailed: {
        const auto m = net::decode<wire::DeviceFailedT>(msg.payload);
        events_.onDeviceFailed(id_, m.device, m.error);
        break;
      }
      case MsgType::Stats: {
        const auto m = net::decode<wire::StatsT>(msg.payload);
        spdlog::debug("{}: CPU {:.0f}%, RAM {:.1f}/{:.1f} GB{}", info_.name, m.cpu_percent, m.ram_used / 1e9,
                      m.ram_total / 1e9, m.upload_bound ? ", upload-bound" : "");
        for (const auto& d : m.devices)
          spdlog::debug("{} device {}: {:.0f}% busy, VRAM {:.1f}/{:.1f} GB, {}, {:.1f} tiles/s, {:.3g} samples/s",
                        info_.name, d->device, d->busy_percent, d->vram_used / 1e9, d->vram_total / 1e9,
                        d->temperature_c > -273 ? fmt::format("{:.0f} C", d->temperature_c) : "temperature unknown",
                        d->tiles_per_s, d->samples_per_s);
        WorkerStats stats;
        stats.cpu_percent = m.cpu_percent;
        stats.ram_used = m.ram_used;
        stats.ram_total = m.ram_total;
        stats.upload_bound = m.upload_bound;
        for (const auto& d : m.devices)
          stats.devices.push_back({d->device, d->busy_percent, d->vram_used, d->vram_total, d->temperature_c,
                                   d->tiles_per_s, d->samples_per_s});
        events_.onStats(id_, stats);
        break;
      }
      case MsgType::Log: {
        const auto m = net::decode<wire::LogT>(msg.payload);
        if (m.level == wire::LogLevel::Error) spdlog::error("{}: {}", info_.name, m.text);
        else if (m.level == wire::LogLevel::Warn) spdlog::warn("{}: {}", info_.name, m.text);
        else spdlog::info("{}: {}", info_.name, m.text);
        break;
      }
      case MsgType::Heartbeat: {
        // Echo at once: the worker measures the round trip, and the reply is the controller's heartbeat.
        const auto m = net::decode<wire::HeartbeatT>(msg.payload);
        wire::HeartbeatT reply;
        reply.sent_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
        reply.echo_us = m.sent_us;
        connection_->send(reply);
        break;
      }
      case MsgType::Bye: {
        const auto m = net::decode<wire::ByeT>(msg.payload);
        std::lock_guard lock(mutex_);
        said_bye_ = true;
        bye_reason_ = m.reason;
        break;
      }
      default:
        spdlog::warn("{}: unexpected {} message; disconnecting", info_.name, net::toString(msg.type));
        connection_->close("protocol error");
        break;
    }
  } catch (const net::ProtocolError& e) {
    spdlog::warn("{}: {} ({}); disconnecting", info_.name, e.what(), net::toString(msg.type));
    connection_->close("protocol error");
  }
}

void RemoteWorker::onClosed(const std::string& reason) {
  std::string why;
  {
    std::lock_guard lock(mutex_);
    if (gone_) return;
    gone_ = true;
    why = said_bye_ ? bye_reason_ : reason;
  }
  events_.onGone(id_, GoneReason::Lost, why);
}

}  // namespace pen
