#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "controller/render_controller.h"
#include "controller/render_job.h"
#include "controller/worker_link.h"
#include "core/settings/render_settings.h"

namespace pen {

class Host;
class RemoteWorker;
class UpnpMapping;

// Something the cluster did, for the Dashboard's event list and the log (docs/GUIDE.md "UI": host joined or left,
// device failed, worker kicked, checkpoint written, errors).
struct SessionEvent {
  enum class Level { Info, Warning, Error };
  std::chrono::system_clock::time_point time;
  Level level = Level::Info;
  std::string text;
};

// One connected (or just disconnected) worker host, as the Hosts pages show it.
struct HostView {
  WorkerId id = 0;
  WorkerInfo info;
  std::chrono::system_clock::time_point since;  // joined
  bool connected = true;
  bool in_render = false;  // attached to the current render
  std::optional<WorkerStats> stats;  // the latest Stats (every 2 s)
  std::vector<SessionEvent> log;     // the latest warnings and errors it forwarded (at most 20)
};

// The controller's hosting session (docs/GUIDE.md "Session versus render"): one listener and join code for as
// long as hosting is on. Workers join it, stay connected while idle and are offered every render that starts;
// a worker joining mid-render joins that render. Renders run one at a time through RenderController with the
// local devices and every connected worker, exactly like a terminal render, so the UI and `--terminal` share one
// code path. Thread-safe: the network thread delivers worker messages, the caller runs render(), and the UI
// reads hosts() and events() from its own thread.
class HostingSession final : public WorkerEvents {
 public:
  HostingSession();
  ~HostingSession() override;

  // Starts accepting workers on settings.port / settings.bind with the canonical join `code`; logs the addresses
  // workers can use and maps the port on the router with UPnP unless --no-upnp. Throws std::runtime_error when
  // the port cannot be opened.
  void startHosting(const RenderSettings& settings, const std::string& code);
  // Closes every worker connection, after waiting up to `grace` for workers to leave on their own (headless
  // workers without --stay say Bye after RenderComplete).
  void stopHosting(std::chrono::milliseconds grace);
  bool hosting() const;
  uint16_t port() const;
  std::string code() const;  // canonical; empty when not hosting
  // Internet hosting (docs/GUIDE.md "Internet workers"): the UPnP port mapping on or off while hosting.
  void setInternet(bool on);
  std::string internetStatus() const;  // "off", or the UPnP outcome
  // Ask before accepting new workers; `approve` answers the question (the UI's dialog).
  void setApproval(bool on, std::function<bool(const std::string&)> approve);

  // Runs one render to the end (blocking) on `localDevices` (none with --no-local) and every connected worker.
  // Throws what RenderController::run throws (every device failed, every worker left).
  RenderOutcome render(RenderJob& job, std::vector<std::unique_ptr<IBackend>> localDevices,
                       const RenderRunOptions& run);
  // The current render's progress, or nothing between renders.
  std::optional<RenderProgress> progress() const;
  // The current render's live view, pause and "save image now" (UI viewport); false between renders.
  bool liveView(LiveView& out, uint64_t version) const;
  bool setPaused(bool paused);
  bool paused() const;
  bool writeImageNow(const RenderSettings& settings, std::string& error);
  bool readFilm(const TileRect& rect, std::vector<Vec3f>& out) const;

  std::vector<HostView> hosts();          // connected first, then the recently gone
  std::vector<SessionEvent> events() const;  // oldest first, at most kMaxEvents
  void addEvent(SessionEvent::Level level, std::string text);

  static constexpr size_t kMaxEvents = 50;

  // WorkerEvents: messages from remote workers (network thread), forwarded to the current render.
  void onReady(WorkerId worker, const std::vector<DeviceReady>& devices) override;
  void onRequestWork(WorkerId worker, DeviceId device, int count) override;
  void onTileResult(WorkerId worker, TileResult&& result) override;
  void onRelease(WorkerId worker, std::span<const LeaseId> leases) override;
  void onDeviceFailed(WorkerId worker, DeviceId device, const std::string& error) override;
  void onGone(WorkerId worker, GoneReason reason, const std::string& detail) override;
  void onStats(WorkerId worker, const WorkerStats& stats) override;
  void onLog(WorkerId worker, LogLevel level, const std::string& text) override;
  // Disconnects a worker with a reason (the Hosts page's Kick).
  void kick(WorkerId worker, const std::string& reason);
  // Hosts and devices switched off on the Hosts pages: a disabled device's work requests are dropped, a disabled
  // host is not offered renders (it stays connected). Applies to renders that start afterwards.
  void setDisabled(std::set<WorkerId> hosts, std::set<std::pair<WorkerId, DeviceId>> devices);

 private:
  struct Entry {
    std::unique_ptr<RemoteWorker> link;
    HostView view;
    std::optional<WorkerId> render_id;  // its id in the current render, when attached
  };

  // A remote worker accepted by the Host: the session owns it; it joins the current render, if any.
  void onJoin(const std::function<std::unique_ptr<WorkerLink>(WorkerId)>& make);
  // Attaches a connected worker to the running render (offers it the scene). Takes the session lock itself.
  void attach(WorkerId id);
  std::optional<WorkerId> renderId(WorkerId id) const;
  // Drops gone workers that no render holds any more (not on the network thread's call stack: they are only
  // marked gone there).
  void pruneLocked();

  mutable std::mutex mutex_;
  // Guards the current render's lifetime: worker messages are forwarded under a shared lock, and the render is
  // detached under an exclusive one, so no message reaches a controller that is being destroyed.
  mutable std::shared_mutex render_mutex_;
  std::unique_ptr<Host> host_;
  std::unique_ptr<UpnpMapping> upnp_;
  std::string code_;
  std::map<WorkerId, Entry> workers_;
  WorkerId next_id_ = 1;
  std::deque<SessionEvent> events_;
  std::set<WorkerId> disabled_hosts_;
  std::set<std::pair<WorkerId, DeviceId>> disabled_devices_;
  RenderController* render_ = nullptr;  // while render() runs
};

}  // namespace pen
