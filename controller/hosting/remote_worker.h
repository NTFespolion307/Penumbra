#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "controller/hosting/scene_uploader.h"
#include "controller/worker_link.h"
#include "net/connection.h"

namespace pen {

// A worker on another process or host, behind the same WorkerLink as the local worker (docs/GUIDE.md
// "Controller and workers"). Messages from the worker arrive on the network thread through onMessage().
class RemoteWorker final : public WorkerLink {
 public:
  RemoteWorker(WorkerId id, WorkerEvents& events, SceneUploader& uploader,
               std::shared_ptr<net::Connection> connection, net::wire::HelloT hello, uint64_t sessionId);
  ~RemoteWorker() override;

  // Network thread: one message from this worker, or the connection closing.
  void onMessage(net::Message&& msg);
  void onClosed(const std::string& reason);

  WorkerInfo info() const override { return info_; }
  void offerScene(const RenderOffer& offer) override;
  void sendLease(const Lease& lease) override;
  void cancel(std::span<const LeaseId> leases) override;
  void ackTile(LeaseId lease, AckStatus status) override;
  void pause(bool paused) override;
  void complete() override;
  void kick(const std::string& reason) override;
  uint32_t itemSpan() const override { return span_.load(); }

  bool connected() const { return connection_->isOpen(); }
  // Closes the connection; the worker is reported gone with `reason`.
  void disconnect(const std::string& reason);

 private:
  TileResult decodeResult(const net::wire::TileResultT& m) const;

  WorkerId id_;
  WorkerEvents& events_;
  SceneUploader& uploader_;
  std::shared_ptr<net::Connection> connection_;
  std::shared_ptr<const ScenePackage> package_;
  uint64_t package_hash_ = 0;
  WorkerInfo info_;
  net::wire::HelloT hello_;
  uint64_t render_id_ = 0;
  uint64_t settings_hash_ = 0;
  // Upload-bound workers get several chunks per item (D-116): set from Stats on the network thread.
  void adaptSpan(uint64_t queued, double bytesPerSecond);
  std::atomic<uint32_t> span_{1};
  std::chrono::steady_clock::time_point span_changed_{};
  std::chrono::steady_clock::time_point backlog_low_since_{};
  int behind_reports_ = 0;  // Stats reports in a row with a backlog
  std::mutex mutex_;
  bool gone_ = false;
  bool said_bye_ = false;
  std::string bye_reason_;
};

}  // namespace pen
