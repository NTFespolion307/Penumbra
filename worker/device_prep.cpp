#include "worker/device_prep.h"

#include <chrono>

namespace pen {

std::vector<PreparedDevice> prepareDevices(const Scene& scene, const RenderSettings& settings,
                                           std::vector<std::unique_ptr<IBackend>>& devices) {
  std::vector<std::string> notes;  // the controller logs camera notes itself
  FrameSetup frame;
  frame.camera = RenderCamera(scene, settings, settings.outputWidth(), settings.outputHeight(), notes);
  frame.params = IntegratorParams::from(settings, scene);
  frame.seed = settings.seed;

  std::vector<PreparedDevice> ready;
  const SceneStats stats = scene.stats();
  for (uint32_t d = 0; d < devices.size(); ++d) {
    const auto start = std::chrono::steady_clock::now();
    const MemoryPlan plan = devices[d]->plan(stats);
    devices[d]->upload(scene, plan);
    devices[d]->prepare(frame);
    PreparedDevice r;
    r.device = d;
    r.backend = devices[d]->info().backend;
    r.memory_tier = plan.tier;
    r.summary = plan.summary;
    r.build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ready.push_back(r);
  }
  return ready;
}

}  // namespace pen
