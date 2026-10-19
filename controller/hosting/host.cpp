#include "controller/hosting/host.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <thread>

#include "core/version.h"
#include "net/join_code.h"

namespace pen {

namespace wire = net::wire;
using Clock = std::chrono::steady_clock;

namespace {
constexpr size_t kMaxFailures = 5;  // more than this many failed handshakes in a minute block the address
constexpr auto kFailureWindow = std::chrono::minutes(1);
constexpr auto kBlockTime = std::chrono::minutes(10);
}  // namespace

struct Host::Peer {
  std::shared_ptr<net::Connection> connection;
  RemoteWorker* worker = nullptr;  // set once the worker is accepted; owned by the render
  wire::HelloT hello;              // kept while it waits for approval
  bool hello_seen = false;
};

Host::Host(const RenderSettings& settings, const std::string& code, WorkerEvents& events, JoinFn onJoin)
    : settings_(settings), events_(events), on_join_(std::move(onJoin)), uploader_(settings.upload_limit) {
  std::random_device rd;
  session_id_ = (uint64_t(rd()) << 32) | rd();
  listener_ = std::make_unique<net::Listener>(io_, settings.bind, settings.port, net::deriveJoinKey(code),
                                              [this](std::shared_ptr<net::Connection> c) { onAccept(std::move(c)); });
  if (settings.approve_workers) approval_thread_ = std::thread([this] { approvalLoop(); });
}

Host::~Host() { shutdown(std::chrono::milliseconds(0)); }

uint16_t Host::port() const { return listener_ ? listener_->port() : 0; }

bool Host::blockedLocked(const std::string& address, Clock::time_point now) {
  auto it = blocked_until_.find(address);
  if (it == blocked_until_.end()) return false;
  if (now < it->second) return true;
  blocked_until_.erase(it);
  return false;
}

void Host::onAccept(std::shared_ptr<net::Connection> connection) {
  auto peer = std::make_shared<Peer>();
  peer->connection = connection;
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    if (blockedLocked(connection->peerAddress(), Clock::now())) {
      spdlog::debug("refused {}: address blocked after repeated wrong join codes", connection->peer());
      connection->close("blocked");
      return;
    }
    peers_.push_back(peer);
  }
  spdlog::debug("connection from {}", connection->peer());
  std::weak_ptr<Peer> weak = peer;
  connection->start({
      [this, weak](net::Message&& msg, uint16_t version) {
        if (auto p = weak.lock()) {
          if (p->worker) p->worker->onMessage(std::move(msg));
          else onHello(p, std::move(msg), version);
        }
      },
      [this, weak](const std::string& reason) {
        if (auto p = weak.lock()) onClosed(p, reason);
      },
  });
}

void Host::onClosed(const std::shared_ptr<Peer>& peer, const std::string& reason) {
  if (peer->worker) peer->worker->onClosed(reason);
  std::lock_guard lock(mutex_);
  std::erase(peers_, peer);
  if (!peer->connection->handshakeFailed()) return;
  // Count failed handshakes per address (docs/GUIDE.md "Join code").
  const std::string& address = peer->connection->peerAddress();
  const auto now = Clock::now();
  auto& times = failures_[address];
  times.push_back(now);
  while (!times.empty() && now - times.front() > kFailureWindow) times.pop_front();
  spdlog::warn("a connection from {} failed the encrypted handshake: wrong join code, or not a Penumbra worker",
               peer->connection->peer());
  if (times.size() > kMaxFailures && !blockedLocked(address, now)) {
    blocked_until_[address] = now + kBlockTime;
    times.clear();
    spdlog::warn("blocking {} for 10 minutes after more than {} failed joins in a minute", address, kMaxFailures);
  }
}

void Host::reject(const std::shared_ptr<Peer>& peer, uint8_t reason, const std::string& text) {
  spdlog::warn("rejected {}: {}", peer->connection->peer(), text);
  wire::RejectT r;
  r.reason = static_cast<wire::RejectReason>(reason);
  r.text = text;
  peer->connection->send(r);
  peer->connection->closeAfterSend("rejected");
}

void Host::onHello(const std::shared_ptr<Peer>& peer, net::Message&& msg, uint16_t version) {
  auto& conn = *peer->connection;
  if (peer->hello_seen) return;  // waiting for approval: nothing else is expected yet
  if (msg.type != net::MsgType::Hello) {
    spdlog::warn("{} did not start with Hello; disconnecting", conn.peer());
    conn.close("protocol error");
    return;
  }
  peer->hello_seen = true;
  if (version != kProtocolVersion)
    return reject(peer, uint8_t(wire::RejectReason::VersionMismatch),
                  fmt::format("protocol version {} does not match the controller's {}; run the same Penumbra version "
                              "on every machine (git pull, then rebuild)",
                              version, kProtocolVersion));
  try {
    peer->hello = net::decode<wire::HelloT>(msg.payload);
  } catch (const net::ProtocolError& e) {
    spdlog::warn("{}: bad Hello ({}); disconnecting", conn.peer(), e.what());
    conn.close("protocol error");
    return;
  }
  const wire::HelloT& hello = peer->hello;
  if (hello.app_version != appVersion() || hello.git_hash != gitHash())
    return reject(peer, uint8_t(wire::RejectReason::VersionMismatch),
                  fmt::format("worker '{}' runs Penumbra {} (git {}), the controller runs {} (git {}); tiles from "
                              "different builds may differ, so every machine must run the same version (git pull, "
                              "then rebuild)",
                              hello.worker_name, hello.app_version, hello.git_hash, appVersion(), gitHash()));
  {
    std::lock_guard lock(mutex_);
    size_t workers = 0;
    for (const auto& p : peers_) workers += p->worker != nullptr;
    if (workers >= settings_.max_workers)
      return reject(peer, uint8_t(wire::RejectReason::SessionFull),
                    fmt::format("the session already has {} workers (--max-workers)", workers));
    if (settings_.approve_workers) {
      approvals_.push_back(peer);
      approval_cv_.notify_one();
      return;  // join() runs once the user answers
    }
  }
  join(peer);
}

void Host::join(const std::shared_ptr<Peer>& peer) {
  const wire::HelloT& hello = peer->hello;
  std::string devices;
  for (const auto& d : hello.devices) devices += fmt::format("{}{}", devices.empty() ? "" : ", ", d->name);
  spdlog::info("worker '{}' joined from {}: {}, {} cores, {:.1f} GB RAM; devices: {}", hello.worker_name,
               peer->connection->peer(), hello.os, hello.logical_cores, hello.ram_total / 1e9,
               devices.empty() ? "none" : devices);
  on_join_([&](WorkerId id) -> std::unique_ptr<WorkerLink> {
    auto link = std::make_unique<RemoteWorker>(id, events_, uploader_, peer->connection, std::move(peer->hello),
                                               session_id_);
    peer->worker = link.get();  // before the scene is offered, so the worker's answer finds its link
    return link;
  });
}

// --approve-workers in the terminal: one question at a time; anything but "y" (or end of input) rejects.
void Host::approvalLoop() {
  for (;;) {
    std::shared_ptr<Peer> peer;
    {
      std::unique_lock lock(mutex_);
      approval_cv_.wait(lock, [&] { return stopped_ || !approvals_.empty(); });
      if (stopped_) return;
      peer = approvals_.front().lock();
      approvals_.pop_front();
    }
    if (!peer || !peer->connection->isOpen()) continue;
    std::fprintf(stderr, "\nAccept worker '%s' from %s (%s, %u cores)? [y/N] ", peer->hello.worker_name.c_str(),
                 peer->connection->peer().c_str(), peer->hello.os.c_str(), peer->hello.logical_cores);
    std::fflush(stderr);
    std::string answer;
    const bool got = static_cast<bool>(std::getline(std::cin, answer));
    const bool yes = got && (answer == "y" || answer == "Y" || answer == "yes");
    asio::post(io_.context(), [this, peer, yes] {
      if (!peer->connection->isOpen()) return;
      if (yes) join(peer);
      else reject(peer, uint8_t(wire::RejectReason::NotApproved), "the controller did not approve this worker");
    });
  }
}

void Host::shutdown(std::chrono::milliseconds grace) {
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
  }
  approval_cv_.notify_all();
  if (listener_) listener_->stop();
  const auto deadline = Clock::now() + grace;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (peers_.empty() || Clock::now() >= deadline) break;
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
  uploader_.stop();
  io_.stop();
  // A pending terminal question blocks in getline; it is detached rather than joined (the process is ending).
  if (approval_thread_.joinable()) approval_thread_.detach();
}

}  // namespace pen
