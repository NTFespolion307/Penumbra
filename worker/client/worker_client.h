#pragma once

#include <atomic>
#include <string>

#include "core/settings/render_settings.h"

namespace pen {

struct WorkerOptions {
  RenderSettings settings;  // this host's own settings: devices, backend, threads, name, cache, --stay
  std::string join;         // address[:port]
  std::string code;
  const std::atomic<bool>* interrupt = nullptr;  // Ctrl+C
};

// `penumbra --worker --join ... --code ...` (docs/GUIDE.md "Inside a worker"): connects to the controller,
// prepares the scene on every device when a render starts, renders the leases it is given and sends the
// tiles back. Returns the exit code: 0 after the render completes, 3 no usable device, 5 interrupted,
// 6 rejected by the controller, 7 connection lost.
int runWorker(const WorkerOptions& options);

}  // namespace pen
