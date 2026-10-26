#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "controller/hosting/hosting_session.h"
#include "core/settings/render_settings.h"

namespace pen::ui {

// Runs one render from the UI on its own thread, exactly like `penumbra --terminal`: prepare (scene package,
// scene, film, session file), the local devices and every connected worker through the hosting session, then the
// output (controller/render_job.h). The UI only starts, stops and watches it.
class RenderRunner {
 public:
  enum class State { Idle, Preparing, Rendering, Done, Stopped, Failed };

  explicit RenderRunner(HostingSession& session) : session_(session) {}
  ~RenderRunner();  // stops a running render and waits for it

  // Starts `settings` on this machine's devices (`localDevices`: a --devices value; empty = none) and the session's
  // workers. Returns false with `why` when a render is already running.
  bool start(const RenderSettings& settings, const std::string& localDevices, std::string& why);
  // Like Ctrl+C: no new work, a checkpoint, and the current image is written; resumable later.
  void stop();

  // Exposure is applied after the film (docs/GUIDE.md "Settings changes"), so it may change during a render: the
  // viewport shows it at once and the output uses the latest value.
  void setExposure(double ev) { exposure_ = ev; }
  double exposure() const { return exposure_.load(); }

  State state() const { return state_.load(); }
  bool busy() const { return state_ == State::Preparing || state_ == State::Rendering; }
  // The outcome text (where the image went, or why it failed); empty while running.
  std::string message() const;
  const RenderSettings& settings() const { return settings_; }  // of the current or last render

 private:
  void run(std::string localDevices);

  HostingSession& session_;
  std::thread thread_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> stop_{false};
  std::atomic<double> exposure_{0.0};
  RenderSettings settings_;
  mutable std::mutex mutex_;
  std::string message_;
};

const char* toString(RenderRunner::State s);

}  // namespace pen::ui
