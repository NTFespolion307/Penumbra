#include "worker/device_prep.h"

#include <chrono>
#include <fmt/format.h>
#include <thread>

namespace pen {

std::vector<PreparedDevice> prepareDevices(const Scene& scene, const RenderSettings& settings,
                                           std::vector<std::unique_ptr<IBackend>>& devices) {
  std::vector<std::string> notes;  // the controller logs camera notes itself
  FrameSetup frame;
  frame.camera = RenderCamera(scene, settings, settings.outputWidth(), settings.outputHeight(), notes);
  frame.params = IntegratorParams::from(settings, scene);
  frame.seed = settings.seed;

  const SceneStats stats = scene.stats();
  std::vector<PreparedDevice> ready(devices.size());
  const auto prepare = [&](uint32_t d) {
    const auto start = std::chrono::steady_clock::now();
    PreparedDevice& r = ready[d];
    r.device = d;
    r.backend = devices[d]->info().backend;
    try {
      const MemoryPlan plan = devices[d]->plan(stats);
      devices[d]->upload(scene, plan);
      devices[d]->prepare(frame);
      r.memory_tier = plan.tier;
      r.summary = plan.summary;
    } catch (const std::exception& e) {
      r.skipped = true;
      r.reason = e.what();
    }
    r.build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  if (devices.size() == 1) {
    prepare(0);
  } else {
    std::vector<std::thread> threads;
    for (uint32_t d = 0; d < devices.size(); ++d) threads.emplace_back(prepare, d);
    for (auto& t : threads) t.join();
  }

  std::string reasons;
  for (const PreparedDevice& r : ready) {
    if (!r.skipped) return ready;
    reasons += fmt::format("{}device {}: {}", reasons.empty() ? "" : "; ", r.device, r.reason);
  }
  throw BackendError(reasons.empty() ? "no render devices" : reasons);
}

}  // namespace pen
