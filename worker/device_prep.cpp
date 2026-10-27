#include "worker/device_prep.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <mutex>
#include <thread>

#include "core/settings/parse.h"
#include "core/system/sysinfo.h"

namespace pen {

namespace {

// Pinned RAM this host can still give to devices: --ram-limit (default 75% of physical RAM) less the scene itself,
// which the worker holds in RAM. Devices take from it one at a time, so two GPUs never count the same bytes.
class RamPool {
 public:
  RamPool(const RenderSettings& settings, const SceneStats& stats) {
    const uint64_t limit = settings.ram_limit ? settings.ram_limit : querySystem().ram_total / 4 * 3;
    const uint64_t scene = stats.geometry_bytes + stats.texture_bytes;
    free_ = limit > scene ? limit - scene : 0;
  }
  // Chooses the placement against what is left and takes its RAM.
  Placement place(const SceneFootprint& fp, MemoryBudgets& b) {
    std::lock_guard lock(mutex_);
    b.ram = free_;
    Placement p = choosePlacement(fp, b);
    free_ -= std::min(free_, p.ram_bytes);
    return p;
  }
  void giveBack(uint64_t bytes) {
    std::lock_guard lock(mutex_);
    free_ += bytes;
  }

 private:
  std::mutex mutex_;
  uint64_t free_ = 0;
};

}  // namespace

std::vector<PreparedDevice> prepareDevices(const Scene& scene, const RenderSettings& settings,
                                           std::vector<std::unique_ptr<IBackend>>& devices) {
  std::vector<std::string> notes;  // the controller logs camera notes itself
  FrameSetup frame;
  frame.camera = RenderCamera(scene, settings, settings.outputWidth(), settings.outputHeight(), notes);
  frame.params = IntegratorParams::from(settings, scene);
  frame.seed = settings.seed;

  const SceneStats stats = scene.stats();
  RamPool ram(settings, stats);
  std::vector<PreparedDevice> ready(devices.size());
  const auto prepare = [&](uint32_t d) {
    const auto start = std::chrono::steady_clock::now();
    PreparedDevice& r = ready[d];
    r.device = d;
    r.backend = devices[d]->info().backend;
    Placement placement;
    try {
      const uint64_t vram = devices[d]->vramBudget();
      if (vram == 0) {
        // The CPU (and the null test device): the scene stays where it is.
        r.summary = fmt::format("tier 0, scene {} in RAM", formatBytes(stats.geometry_bytes + stats.texture_bytes));
      } else {
        MemoryBudgets b;
        b.vram = settings.vram_limit ? std::min(settings.vram_limit, vram) : vram;
        b.allow_downsize = !settings.no_texture_downsize;
        const SceneFootprint fp = devices[d]->measure(scene);
        // An allocation can still fail (another program took VRAM after the budget was read): that tier is given up
        // and the next one tried, so the device renders more slowly instead of crashing (docs/GUIDE.md).
        for (;;) {
          placement = ram.place(fp, b);
          r.summary = describePlacement(placement, b);
          if (placement.tier == 3) throw BackendError(fmt::format("memory tier 3: {}", placement.reason));
          try {
            devices[d]->upload(scene, placement);
            break;
          } catch (const OutOfMemoryError& e) {
            ram.giveBack(placement.ram_bytes);
            spdlog::info("device {}: out of memory at tier {} ({}); trying tier {}", d, placement.tier, e.what(),
                         placement.tier + 1);
            b.min_tier = placement.tier + 1;
            placement = Placement{};
          }
        }
      }
      if (vram == 0) devices[d]->upload(scene, placement);
      devices[d]->prepare(frame);
      r.memory_tier = placement.tier;
      r.vram_bytes = placement.vram_bytes;
      r.ram_bytes = placement.ram_bytes;
    } catch (const std::exception& e) {
      ram.giveBack(placement.ram_bytes);
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
