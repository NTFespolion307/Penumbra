#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "controller/hosting/remote_worker.h"
#include "core/settings/render_settings.h"
#include "net/connection.h"

namespace pen {

// The controller's listening side (docs/GUIDE.md "Controller and workers"): accepts connections, checks each
// worker's Hello (protocol, version and git hash must match exactly) and hands accepted workers to the render.
class Host {
 public:
  // Called on the network thread for each accepted worker; must add the link that `make` creates to the render
  // (RenderController::addWorker).
  using MakeLink = std::function<std::unique_ptr<WorkerLink>(WorkerId)>;
  using JoinFn = std::function<void(const MakeLink& make)>;

  // Starts listening. `events` receives every remote worker's messages. Throws std::runtime_error when the
  // port cannot be opened.
  Host(const RenderSettings& settings, WorkerEvents& events, JoinFn onJoin);
  ~Host();

  uint16_t port() const;
  // Waits up to `grace` for workers to disconnect after RenderComplete, then closes everything and stops.
  void shutdown(std::chrono::milliseconds grace);

 private:
  struct Peer;
  void onAccept(std::shared_ptr<net::Connection> connection);
  void onHello(const std::shared_ptr<Peer>& peer, net::Message&& msg, uint16_t version);

  RenderSettings settings_;
  WorkerEvents& events_;
  JoinFn on_join_;
  uint64_t session_id_ = 0;
  net::IoThread io_;
  std::unique_ptr<net::Listener> listener_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Peer>> peers_;
  bool stopped_ = false;
};

}  // namespace pen
