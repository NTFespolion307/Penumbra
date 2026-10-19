#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controller/hosting/remote_worker.h"
#include "core/settings/render_settings.h"
#include "net/connection.h"

namespace pen {

// The controller's listening side (docs/GUIDE.md "Controller and workers", "Join code"): accepts TLS
// connections keyed by the join code, checks each worker's Hello (protocol, version and git hash must match
// exactly), optionally asks the user to approve it, and hands accepted workers to the render. An address with
// more than 5 failed handshakes in a minute is blocked for 10 minutes.
class Host {
 public:
  // Called on the network thread for each accepted worker; must add the link that `make` creates to the render
  // (RenderController::addWorker).
  using MakeLink = std::function<std::unique_ptr<WorkerLink>(WorkerId)>;
  using JoinFn = std::function<void(const MakeLink& make)>;

  // Starts listening. `code` is the canonical join code; `events` receives every remote worker's messages.
  // Throws std::runtime_error when the port cannot be opened.
  Host(const RenderSettings& settings, const std::string& code, WorkerEvents& events, JoinFn onJoin);
  ~Host();

  uint16_t port() const;
  // Waits up to `grace` for workers to disconnect after RenderComplete, then closes everything and stops.
  void shutdown(std::chrono::milliseconds grace);

 private:
  struct Peer;
  void onAccept(std::shared_ptr<net::Connection> connection);
  void onClosed(const std::shared_ptr<Peer>& peer, const std::string& reason);
  void onHello(const std::shared_ptr<Peer>& peer, net::Message&& msg, uint16_t version);
  void join(const std::shared_ptr<Peer>& peer);
  void reject(const std::shared_ptr<Peer>& peer, uint8_t reason, const std::string& text);
  bool blockedLocked(const std::string& address, std::chrono::steady_clock::time_point now);
  void approvalLoop();

  RenderSettings settings_;
  WorkerEvents& events_;
  JoinFn on_join_;
  uint64_t session_id_ = 0;
  SceneUploader uploader_;
  net::IoThread io_;
  std::unique_ptr<net::Listener> listener_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Peer>> peers_;
  bool stopped_ = false;

  // Failed handshakes per address, and blocked addresses (guarded by mutex_).
  std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> failures_;
  std::map<std::string, std::chrono::steady_clock::time_point> blocked_until_;

  // --approve-workers: questions asked one at a time on the terminal.
  std::thread approval_thread_;
  std::condition_variable approval_cv_;
  std::deque<std::weak_ptr<Peer>> approvals_;
};

}  // namespace pen
