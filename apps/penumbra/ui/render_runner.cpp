#include "apps/penumbra/ui/render_runner.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "controller/render_job.h"
#include "core/exit_codes.h"
#include "core/system/paths.h"
#include "worker/device_manager.h"

namespace pen::ui {

RenderRunner::~RenderRunner() {
  stop();
  if (thread_.joinable()) thread_.join();
}

bool RenderRunner::start(const RenderSettings& settings, const std::string& localDevices, std::string& why) {
  if (busy()) {
    why = "a render is already running";
    return false;
  }
  if (thread_.joinable()) thread_.join();  // the previous render's thread has finished
  settings_ = settings;
  stop_ = false;
  {
    std::lock_guard lock(mutex_);
    message_.clear();
  }
  state_ = State::Preparing;
  thread_ = std::thread([this, localDevices] { run(localDevices); });
  return true;
}

void RenderRunner::stop() { stop_ = true; }

std::string RenderRunner::message() const {
  std::lock_guard lock(mutex_);
  return message_;
}

void RenderRunner::run(std::string localDevices) {
  const auto finish = [&](State s, std::string text) {
    {
      std::lock_guard lock(mutex_);
      message_ = std::move(text);
    }
    state_ = s;
    session_.addEvent(s == State::Failed ? SessionEvent::Level::Error : SessionEvent::Level::Info, message());
  };
  RenderSettings s = settings_;
  session_.addEvent(SessionEvent::Level::Info,
                    fmt::format("Render started \xC2\xB7 {} \xC2\xB7 {}x{} \xC2\xB7 {} spp", pathToUtf8(s.scene.filename()),
                                s.outputWidth(), s.outputHeight(), s.spp));
  RenderJob job;
  if (const int rc = prepareNewRender(s, job); rc >= 0) {
    finish(State::Failed, fmt::format("The render could not start (exit code {}); the log says why", rc));
    return;
  }

  // This machine's devices, as on the Settings page; none when everything local is switched off.
  DeviceSelection devices;
  if (!localDevices.empty()) {
    RenderSettings ds = s;
    ds.devices = localDevices;
    try {
      devices = createDevices(ds, /*controllerHost=*/true);
      for (const auto& n : devices.notes) spdlog::info("{}", n);
    } catch (const std::exception& e) {
      spdlog::warn("UI: this machine's devices cannot render: {}", e.what());
      session_.addEvent(SessionEvent::Level::Warning, fmt::format("This machine renders nothing: {}", e.what()));
    }
  }
  if (devices.devices.empty() && !session_.hosting()) {
    finish(State::Failed, "Nothing can render: this machine's devices are off and it is not hosting workers");
    return;
  }

  state_ = State::Rendering;
  RenderRunOptions run;
  run.interrupt = &stop_;
  run.after_checkpoint = [&](const RenderProgress& p) {
    saveSessionProgress(job, p);
    session_.addEvent(SessionEvent::Level::Info, fmt::format("Checkpoint written \xC2\xB7 {} of {} tiles done",
                                                            p.items_done, p.items_total));
  };
  RenderOutcome outcome;
  try {
    outcome = session_.render(job, std::move(devices.devices), run);
  } catch (const std::exception& e) {
    finish(State::Failed, fmt::format("The render failed: {}", e.what()));
    return;
  }
  const int rc = finishRender(job, outcome);
  if (rc != toInt(ExitCode::Success) && rc != toInt(ExitCode::Interrupted)) {
    finish(State::Failed, fmt::format("The image could not be written (exit code {}); the log says why", rc));
    return;
  }
  finish(outcome.complete ? State::Done : State::Stopped,
         fmt::format("{} in {}; wrote {}", outcome.complete ? "Done" : "Stopped", fmt::format("{:.0f} s", outcome.seconds),
                     pathToUtf8(s.output)));
}

const char* toString(RenderRunner::State s) {
  switch (s) {
    case RenderRunner::State::Idle: return "Idle";
    case RenderRunner::State::Preparing: return "Preparing";
    case RenderRunner::State::Rendering: return "Rendering";
    case RenderRunner::State::Done: return "Done";
    case RenderRunner::State::Stopped: return "Stopped";
    case RenderRunner::State::Failed: return "Failed";
  }
  return "?";
}

}  // namespace pen::ui
