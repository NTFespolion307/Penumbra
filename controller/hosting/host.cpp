#include "controller/hosting/host.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <random>
#include <thread>

#include "core/version.h"

namespace pen {

namespace wire = net::wire;

struct Host::Peer {
  std::shared_ptr<net::Connection> connection;
  RemoteWorker* worker = nullptr;  // set once the Hello is accepted; owned by the render
  bool closed = false;
};

Host::Host(const RenderSettings& settings, WorkerEvents& events, JoinFn onJoin)
    : settings_(settings), events_(events), on_join_(std::move(onJoin)) {
  std::random_device rd;
  session_id_ = (uint64_t(rd()) << 32) | rd();
  listener_ = std::make_unique<net::Listener>(io_, settings.bind, settings.port,
                                              [this](std::shared_ptr<net::Connection> c) { onAccept(std::move(c)); });
}

Host::~Host() { shutdown(std::chrono::milliseconds(0)); }

uint16_t Host::port() const { return listener_ ? listener_->port() : 0; }

void Host::onAccept(std::shared_ptr<net::Connection> connection) {
  auto peer = std::make_shared<Peer>();
  peer->connection = connection;
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    peers_.push_back(peer);
  }
  spdlog::debug("connection from {}", connection->peer());
  std::weak_ptr<Peer> weak = peer;
  connection->start({
      [this, weak](net::Message&& msg, uint16_t version) {
        auto p = weak.lock();
        if (!p) return;
        if (p->worker) p->worker->onMessage(std::move(msg));
        else onHello(p, std::move(msg), version);
      },
      [this, weak](const std::string& reason) {
        auto p = weak.lock();
        if (!p) return;
        if (p->worker) p->worker->onClosed(reason);
        std::lock_guard lock(mutex_);
        p->closed = true;
        std::erase(peers_, p);
      },
  });
}

void Host::onHello(const std::shared_ptr<Peer>& peer, net::Message&& msg, uint16_t version) {
  auto& conn = *peer->connection;
  const auto reject = [&](wire::RejectReason reason, const std::string& text) {
    spdlog::warn("rejected {}: {}", conn.peer(), text);
    wire::RejectT r;
    r.reason = reason;
    r.text = text;
    conn.send(r);
    conn.closeAfterSend("rejected");
  };
  if (msg.type != net::MsgType::Hello) {
    spdlog::warn("{} did not start with Hello; disconnecting", conn.peer());
    conn.close("protocol error");
    return;
  }
  if (version != kProtocolVersion)
    return reject(wire::RejectReason::VersionMismatch,
                  fmt::format("protocol version {} does not match the controller's {}; run the same Penumbra version "
                              "on every machine (git pull, then rebuild)",
                              version, kProtocolVersion));
  wire::HelloT hello;
  try {
    hello = net::decode<wire::HelloT>(msg.payload);
  } catch (const net::ProtocolError& e) {
    spdlog::warn("{}: bad Hello ({}); disconnecting", conn.peer(), e.what());
    conn.close("protocol error");
    return;
  }
  if (hello.app_version != appVersion() || hello.git_hash != gitHash())
    return reject(wire::RejectReason::VersionMismatch,
                  fmt::format("worker '{}' runs Penumbra {} (git {}), the controller runs {} (git {}); tiles from "
                              "different builds may differ, so every machine must run the same version (git pull, "
                              "then rebuild)",
                              hello.worker_name, hello.app_version, hello.git_hash, appVersion(), gitHash()));
  {
    std::lock_guard lock(mutex_);
    size_t workers = 0;
    for (const auto& p : peers_) workers += p->worker != nullptr;
    if (workers >= settings_.max_workers)
      return reject(wire::RejectReason::SessionFull,
                    fmt::format("the session already has {} workers (--max-workers)", workers));
  }

  std::string devices;
  for (const auto& d : hello.devices) devices += fmt::format("{}{}", devices.empty() ? "" : ", ", d->name);
  spdlog::info("worker '{}' joined from {}: {}, {} cores, {:.1f} GB RAM; devices: {}", hello.worker_name, conn.peer(),
               hello.os, hello.logical_cores, hello.ram_total / 1e9, devices.empty() ? "none" : devices);
  on_join_([&](WorkerId id) -> std::unique_ptr<WorkerLink> {
    auto link = std::make_unique<RemoteWorker>(id, events_, peer->connection, std::move(hello), session_id_);
    peer->worker = link.get();  // before the scene is offered, so the worker's answer finds its link
    return link;
  });
}

void Host::shutdown(std::chrono::milliseconds grace) {
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
  }
  if (listener_) listener_->stop();
  const auto deadline = std::chrono::steady_clock::now() + grace;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (peers_.empty() || std::chrono::steady_clock::now() >= deadline) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::vector<std::shared_ptr<Peer>> left;
  {
    std::lock_guard lock(mutex_);
    left = peers_;
  }
  for (auto& p : left) p->connection->close("the controller is shutting down");
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard lock(mutex_);
      if (peers_.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  io_.stop();
}

}  // namespace pen
