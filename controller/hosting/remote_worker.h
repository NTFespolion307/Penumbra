#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "controller/worker_link.h"
#include "net/connection.h"

namespace pen {

// A worker on another process or host, behind the same WorkerLink as the local worker (docs/GUIDE.md
// "Controller and workers"). Messages from the worker arrive on the network thread through onMessage().
class RemoteWorker final : public WorkerLink {
 public:
  RemoteWorker(WorkerId id, WorkerEvents& events, std::shared_ptr<net::Connection> connection,
               net::wire::HelloT hello, uint64_t sessionId);
  ~RemoteWorker() override;

  // Network thread: one message from this worker, or the connection closing.
  void onMessage(net::Message&& msg);
  void onClosed(const std::string& reason);

  WorkerInfo info() const override { return info_; }
  void offerScene(std::shared_ptr<const Scene> scene, const RenderSettings& settings) override;
  void sendLease(const Lease& lease) override;
  void cancel(std::span<const LeaseId> leases) override;
  void ackTile(LeaseId lease, AckStatus status) override;
  void pause(bool paused) override;
  void complete() override;

  bool connected() const { return connection_->isOpen(); }
  // Closes the connection; the worker is reported gone with `reason`.
  void disconnect(const std::string& reason);

 private:
  TileResult decodeResult(const net::wire::TileResultT& m) const;

  WorkerId id_;
  WorkerEvents& events_;
  std::shared_ptr<net::Connection> connection_;
  WorkerInfo info_;
  net::wire::HelloT hello_;
  uint64_t render_id_ = 0;
  uint64_t settings_hash_ = 0;
  std::mutex mutex_;
  bool gone_ = false;
  bool said_bye_ = false;
  std::string bye_reason_;
};

}  // namespace pen
