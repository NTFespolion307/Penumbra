#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/backend.h"
#include "core/settings/render_settings.h"

namespace pen {

// No device matches --backend / --devices; maps to exit code 3.
struct NoDeviceError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct DeviceSelection {
  std::vector<std::unique_ptr<IBackend>> devices;
  std::vector<std::string> notes;  // every automatic decision, for the log (rule: no silent fallbacks)
};

// Enumerates this machine's devices and creates one backend per selected device. On the controller host the
// CPU device leaves 2 threads free when --threads is not given (docs/GUIDE.md "Controller and workers").
DeviceSelection createDevices(const RenderSettings& settings, bool controllerHost);

}  // namespace pen
