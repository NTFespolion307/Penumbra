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
  double build_seconds = 0;
  std::string summary;
};

// Plans memory, uploads the scene and prepares the frame on every device (docs/GUIDE.md "Ready before work").
// Used by the controller's local worker and by network workers. Throws BackendError or SceneError.
std::vector<PreparedDevice> prepareDevices(const Scene& scene, const RenderSettings& settings,
                                           std::vector<std::unique_ptr<IBackend>>& devices);

}  // namespace pen
