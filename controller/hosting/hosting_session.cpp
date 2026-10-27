#include "controller/hosting/hosting_session.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>

#include "controller/hosting/host.h"
#include "controller/hosting/remote_worker.h"
#include "controller/hosting/upnp.h"
#include "controller/local_worker.h"
#include "net/interfaces.h"
#include "net/join_code.h"

namespace pen {

namespace {

using Clock = std::chrono::steady_clock;

// Gone workers stay listed this long (the Hosts page shows them as lost), and are never deleted sooner than
// this after the network thread reported them gone.
constexpr auto kKeepGone = std::chrono::minutes(2);
constexpr size_t kMaxGone = 32;

// The current render's view of a worker the session owns: everything is forwarded, and destroying it when the
// render ends keeps the connection, so the worker stays in the session for the next render.
class AttachedLink final : public WorkerLink {
 public:
  explicit AttachedLink(WorkerLink& worker) : w_(worker) {}
  WorkerInfo info() const override { return w_.info(); }
  void offerScene(const RenderOffer& offer) override { w_.offerScene(offer); }
  void sendLease(const Lease& lease) override { w_.sendLease(lease); }
  void cancel(std::span<const LeaseId> leases) override { w_.cancel(leases); }
  void ackTile(LeaseId lease, AckStatus status) override { w_.ackTile(lease, status); }
  void pause(bool paused) override { w_.pause(paused); }
  void complete() override { w_.complete(); }
  void kick(const std::string& reason) override { w_.kick(reason); }

 private:
  WorkerLink& w_;
};

}  // namespace

HostingSession::HostingSession() = default;

HostingSession::~HostingSession() {
  if (host_) stopHosting(std::chrono::milliseconds(0));
}

// ---------------------------------------------------------------- hosting

void HostingSession::startHosting(const RenderSettings& s, const std::string& code) {
  if (host_) throw std::runtime_error("already hosting");
  auto host = std::make_unique<Host>(s, code, *this, [this](const Host::MakeLink& make) { onJoin(make); });
  const uint16_t port = host->port();
  spdlog::info("hosting on port {}{}; workers join with: penumbra --worker --join <this machine's address>:{} "
               "--code {}",
               port, s.bind.empty() ? " (all interfaces)" : fmt::format(" ({})", s.bind), port,
               net::formatJoinCode(code));
  if (s.approve_workers) spdlog::info("--approve-workers: each new worker waits for your answer here");
  // Where workers can reach this machine (docs/GUIDE.md "Addresses, NAT and firewall").
  for (const net::HostAddress& a : net::listHostAddresses())
    spdlog::info("  {} address {} ({}): penumbra --worker --join {}:{} --code {}", net::toString(a.kind), a.address,
                 a.interface, a.address.find(':') != std::string::npos ? "[" + a.address + "]" : a.address, port,
                 net::formatJoinCode(code));
  std::unique_ptr<UpnpMapping> upnp;
  if (s.no_upnp) spdlog::info("--no-upnp: not asking the router to forward TCP {}", port);
  else if (s.bind.rfind("127.", 0) == 0 || s.bind == "::1") spdlog::info("listening on loopback only: no UPnP");
  else upnp = std::make_unique<UpnpMapping>(port);
  {
    std::lock_guard lock(mutex_);
    host_ = std::move(host);
    upnp_ = std::move(upnp);
    code_ = code;
  }
  addEvent(SessionEvent::Level::Info, fmt::format("hosting on port {}", port));
}

void HostingSession::stopHosting(std::chrono::milliseconds grace) {
  if (!host_) return;
  {
    std::shared_lock rl(render_mutex_);
    if (render_) throw std::logic_error("stopHosting during a render");
  }
  host_->shutdown(grace);
  std::unique_ptr<Host> host;
  {
    std::lock_guard lock(mutex_);
    workers_.clear();  // before the host: the links hold sockets of its network thread
    host = std::move(host_);
    upnp_.reset();
    code_.clear();
  }
  host.reset();
  addEvent(SessionEvent::Level::Info, "hosting stopped");
}

void HostingSession::setInternet(bool on) {
  std::lock_guard lock(mutex_);
  if (!host_) return;
  if (on && !upnp_) upnp_ = std::make_unique<UpnpMapping>(host_->port());
  if (!on) upnp_.reset();  // removes the mapping
}

std::string HostingSession::internetStatus() const {
  std::lock_guard lock(mutex_);
  return upnp_ ? upnp_->status() : std::string("off");
}

void HostingSession::setApproval(bool on, std::function<bool(const std::string&)> approve) {
  std::lock_guard lock(mutex_);
  if (!host_) return;
  host_->setApprover(std::move(approve));
  host_->setApproveWorkers(on);
}

bool HostingSession::hosting() const {
  std::lock_guard lock(mutex_);
  return host_ != nullptr;
}

uint16_t HostingSession::port() const {
  std::lock_guard lock(mutex_);
  return host_ ? host_->port() : 0;
}

std::string HostingSession::code() const {
  std::lock_guard lock(mutex_);
  return code_;
}

void HostingSession::onJoin(const std::function<std::unique_ptr<WorkerLink>(WorkerId)>& make) {
  WorkerId id = 0;
  {
    std::lock_guard lock(mutex_);
    id = next_id_++;
  }
  std::unique_ptr<WorkerLink> link = make(id);  // a RemoteWorker (Host::join); it sends Welcome
  auto* remote = static_cast<RemoteWorker*>(link.release());
  const WorkerInfo info = remote->info();
  {
    std::lock_guard lock(mutex_);
    Entry& e = workers_[id];
    e.link.reset(remote);
    e.view.id = id;
    e.view.info = info;
    e.view.since = std::chrono::system_clock::now();
  }
  addEvent(SessionEvent::Level::Info, fmt::format("{} joined from {}", info.name, info.address));
  attach(id);
}

// ---------------------------------------------------------------- renders

RenderOutcome HostingSession::render(RenderJob& job, std::vector<std::unique_ptr<IBackend>> localDevices,
                                     const RenderRunOptions& run) {
  // RAM films refine the whole image pass by pass; disk films finish tile after tile (docs/GUIDE.md "Pass order").
  const PassOrder order = job.film->inRam() ? PassOrder::BreadthFirst : PassOrder::DepthFirst;
  spdlog::info("pass order: {}", order == PassOrder::BreadthFirst ? "breadth-first (whole image, pass by pass)"
                                                                  : "depth-first (tile after tile)");
  RenderController controller(job.settings, job.scene, job.package, *job.film, order);
  if (const uint32_t restored = controller.restoreFromFilm(); restored > 0) {
    const RenderProgress p = controller.progress();
    spdlog::info("resume: {} of {} tiles already have samples in the film ({:.1f}% of the samples)", restored,
                 p.items_total, p.samples_total ? 100.0 * double(p.samples_done) / double(p.samples_total) : 0.0);
  }
  if (!localDevices.empty())
    controller.addWorker([&](WorkerId id) {
      std::unique_lock wl(render_mutex_);
      local_render_id_ = id;
      return std::make_unique<LocalWorker>(id, controller, std::move(localDevices));
    });

  std::vector<WorkerId> ids;
  {
    std::unique_lock wl(render_mutex_);
    std::lock_guard lock(mutex_);
    controller.setHosting(host_ != nullptr);
    render_ = &controller;
    for (const auto& [id, e] : workers_)
      if (e.view.connected) ids.push_back(id);
  }
  for (WorkerId id : ids) attach(id);

  // Detach before the controller goes away (also when run() throws): no more messages are forwarded to it, and
  // its links (forwarders) die with it while the workers stay connected.
  struct Detach {
    HostingSession& s;
    ~Detach() {
      std::unique_lock wl(s.render_mutex_);
      std::lock_guard lock(s.mutex_);
      s.render_ = nullptr;
      s.local_render_id_.reset();
      for (auto& [id, e] : s.workers_) {
        e.render_id.reset();
        e.view.in_render = false;
      }
    }
  } detach{*this};
  return controller.run(run);
}

std::optional<RenderProgress> HostingSession::progress() const {
  std::shared_lock rl(render_mutex_);
  if (!render_) return std::nullopt;
  return render_->progress();
}

bool HostingSession::liveView(LiveView& out, uint64_t version) const {
  std::shared_lock rl(render_mutex_);
  return render_ && render_->liveView(out, version);
}

bool HostingSession::setPaused(bool paused) {
  std::shared_lock rl(render_mutex_);
  if (!render_) return false;
  render_->setPaused(paused);
  return true;
}

bool HostingSession::paused() const {
  std::shared_lock rl(render_mutex_);
  return render_ && render_->paused();
}

bool HostingSession::writeImageNow(const RenderSettings& settings, std::string& error) {
  std::shared_lock rl(render_mutex_);
  if (!render_) {
    error = "no render is running";
    return false;
  }
  return render_->writeImageNow(settings, error);
}

bool HostingSession::readFilm(const TileRect& rect, std::vector<Vec3f>& out) const {
  std::shared_lock rl(render_mutex_);
  if (!render_) return false;
  render_->readFilm(rect, out);
  return true;
}

void HostingSession::attach(WorkerId id) {
  std::shared_lock rl(render_mutex_);
  if (!render_) return;
  WorkerLink* link = nullptr;
  {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end() || !it->second.view.connected || it->second.render_id) return;
    if (disabled_hosts_.count(id)) return;  // switched off on the Hosts page
    link = it->second.link.get();
  }
  // The render offers the scene right after the link is made (a worker joining mid-render gets it at once).
  render_->addWorker([&](WorkerId rid) {
    {
      std::lock_guard lock(mutex_);
      Entry& e = workers_[id];
      e.render_id = rid;
      e.view.in_render = true;
    }
    return std::make_unique<AttachedLink>(*link);
  });
}

std::optional<WorkerId> HostingSession::renderId(WorkerId id) const {
  std::lock_guard lock(mutex_);
  auto it = workers_.find(id);
  return it == workers_.end() ? std::nullopt : it->second.render_id;
}

// ---------------------------------------------------------------- worker events (network thread)

std::vector<DeviceReady> HostingSession::devicePlacement(WorkerId host) const {
  std::shared_lock rl(render_mutex_);
  if (!render_) return {};
  const std::optional<WorkerId> rid = host == 0 ? local_render_id_ : renderId(host);
  return rid ? render_->deviceReadiness(*rid) : std::vector<DeviceReady>{};
}

void HostingSession::onReady(WorkerId worker, const std::vector<DeviceReady>& devices) {
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onReady(*rid, devices);
}

void HostingSession::onRequestWork(WorkerId worker, DeviceId device, int count) {
  {
    std::lock_guard lock(mutex_);
    if (disabled_devices_.count({worker, device})) return;  // switched off: it gets no work
  }
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onRequestWork(*rid, device, count);
}

void HostingSession::onTileResult(WorkerId worker, TileResult&& result) {
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onTileResult(*rid, std::move(result));
}

void HostingSession::onRelease(WorkerId worker, std::span<const LeaseId> leases) {
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onRelease(*rid, leases);
}

void HostingSession::onDeviceFailed(WorkerId worker, DeviceId device, const std::string& error) {
  std::string name;
  {
    std::lock_guard lock(mutex_);
    if (auto it = workers_.find(worker); it != workers_.end()) name = it->second.view.info.name;
  }
  addEvent(SessionEvent::Level::Error, fmt::format("{} device {} failed: {}", name, device, error));
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onDeviceFailed(*rid, device, error);
}

void HostingSession::onGone(WorkerId worker, GoneReason reason, const std::string& detail) {
  std::string name;
  {
    std::lock_guard lock(mutex_);
    if (auto it = workers_.find(worker); it != workers_.end()) {
      it->second.view.connected = false;
      it->second.view.since = std::chrono::system_clock::now();  // since when it is gone
      name = it->second.view.info.name;
    }
  }
  addEvent(reason == GoneReason::Kicked ? SessionEvent::Level::Warning : SessionEvent::Level::Info,
           fmt::format("{} left ({})", name, detail));
  std::shared_lock rl(render_mutex_);
  if (const auto rid = renderId(worker); render_ && rid) render_->onGone(*rid, reason, detail);
}

void HostingSession::onStats(WorkerId worker, const WorkerStats& stats) {
  std::lock_guard lock(mutex_);
  if (auto it = workers_.find(worker); it != workers_.end()) it->second.view.stats = stats;
}

void HostingSession::onLog(WorkerId worker, LogLevel level, const std::string& text) {
  std::lock_guard lock(mutex_);
  auto it = workers_.find(worker);
  if (it == workers_.end()) return;
  auto& log = it->second.view.log;
  log.push_back({std::chrono::system_clock::now(),
                 level == LogLevel::Error     ? SessionEvent::Level::Error
                 : level == LogLevel::Warning ? SessionEvent::Level::Warning
                                              : SessionEvent::Level::Info,
                 text});
  if (log.size() > 20) log.erase(log.begin());
}

void HostingSession::kick(WorkerId worker, const std::string& reason) {
  RemoteWorker* link = nullptr;
  {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(worker);
    if (it == workers_.end() || !it->second.view.connected) return;
    link = it->second.link.get();
  }
  spdlog::warn("kicking {}: {}", link->info().name, reason);
  link->kick(reason);
}

void HostingSession::setDisabled(std::set<WorkerId> hosts, std::set<std::pair<WorkerId, DeviceId>> devices) {
  std::lock_guard lock(mutex_);
  disabled_hosts_ = std::move(hosts);
  disabled_devices_ = std::move(devices);
}

// ---------------------------------------------------------------- views

void HostingSession::pruneLocked() {
  const auto now = std::chrono::system_clock::now();
  size_t gone = 0;
  for (auto it = workers_.begin(); it != workers_.end();) {
    const Entry& e = it->second;
    const bool removable = !e.view.connected && !e.render_id;
    const auto age = now - e.view.since;
    if (removable && (age > kKeepGone || (gone >= kMaxGone && age > std::chrono::seconds(2)))) {
      it = workers_.erase(it);
      continue;
    }
    gone += !e.view.connected;
    ++it;
  }
}

std::vector<HostView> HostingSession::hosts() {
  std::lock_guard lock(mutex_);
  pruneLocked();
  std::vector<HostView> v;
  for (const auto& [id, e] : workers_) v.push_back(e.view);
  std::stable_partition(v.begin(), v.end(), [](const HostView& h) { return h.connected; });
  return v;
}

void HostingSession::addEvent(SessionEvent::Level level, std::string text) {
  std::lock_guard lock(mutex_);
  events_.push_back({std::chrono::system_clock::now(), level, std::move(text)});
  while (events_.size() > kMaxEvents) events_.pop_front();
}

std::vector<SessionEvent> HostingSession::events() const {
  std::lock_guard lock(mutex_);
  return {events_.begin(), events_.end()};
}

}  // namespace pen
