#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backends/backend.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

// One device after it has the scene: what WorkerReady reports per device.
struct PreparedDevice {
  uint32_t device = 0;
  BackendKind backend = BackendKind::Cpu;
  uint32_t memory_tier = 0;
  uint64_t vram_bytes = 0, ram_bytes = 0;  // where the scene went
  double build_seconds = 0;
  std::string summary;
  bool skipped = false;  // the device could not take the scene (reason says why); it gets no slot
  std::string reason;
};

// Places the scene on every device (the memory tier ladder: each device's live
// VRAM, and one pinned-RAM pool per host that the devices draw from in turn), uploads it and prepares the frame, all
// devices at once, each on its own thread. A device that fails is skipped with its reason and the others go on.
// Used by the controller's local worker and by network
// workers. Throws BackendError when no device is left, SceneError when the camera cannot be set up.
std::vector<PreparedDevice> prepareDevices(const Scene& scene, const RenderSettings& settings,
                                           std::vector<std::unique_ptr<IBackend>>& devices);

}  // namespace pen
