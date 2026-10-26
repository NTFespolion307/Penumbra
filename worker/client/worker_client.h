#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "core/settings/render_settings.h"

namespace pen {

// What the UI's Join a render screen shows (docs/GUIDE.md "Settings page": the controller it joined, scene
// download, its GPUs with usage and tiles done, upload rate). The worker updates it as it goes; read under `mutex`.
struct WorkerStatus {
  std::mutex mutex;
  std::string phase;       // "Connecting", "Waiting for a render", "Downloading the scene", "Rendering", ...
  std::string controller;  // the address it joined
  std::string scene;       // the current render's scene file
  double download = 0;     // 0-1 while the scene downloads
  struct Device {
    std::string name, backend;
    float busy_percent = 0;
    uint64_t tiles = 0;  // this render
  };
  std::vector<Device> devices;
  double upload_bytes_per_s = 0;
  std::string error;  // why it stopped, if it did
};

struct WorkerOptions {
  RenderSettings settings;  // this host's own settings: devices, backend, threads, name, cache, --stay
  std::string join;         // address[:port]
  std::string code;
  const std::atomic<bool>* interrupt = nullptr;  // Ctrl+C
  WorkerStatus* status = nullptr;                // the UI's Join a render screen, when it runs the worker
};

// `penumbra --worker --join ... --code ...` (docs/GUIDE.md "Inside a worker"): connects to the controller,
// prepares the scene on every device when a render starts, renders the leases it is given and sends the
// tiles back. Returns the exit code: 0 after the render completes, 3 no usable device, 5 interrupted,
// 6 rejected by the controller, 7 connection lost.
int runWorker(const WorkerOptions& options);

}  // namespace pen
