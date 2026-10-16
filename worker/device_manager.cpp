#include "worker/device_manager.h"

#include <algorithm>
#include <fmt/format.h>
#include <thread>

#include "backends/cpu/cpu_backend.h"

namespace pen {

DeviceSelection createDevices(const RenderSettings& s, bool controllerHost) {
  DeviceSelection sel;

  switch (s.backend) {
    case BackendKind::Vulkan: throw NoDeviceError("--backend vulkan is built in milestone M5; use --backend cpu");
    case BackendKind::Cuda: throw NoDeviceError("--backend cuda is built in milestone M6; use --backend cpu");
    case BackendKind::Optix: throw NoDeviceError("--backend optix is built in milestone M13; use --backend cpu");
    default: break;
  }

  // --devices: "all", "gpu", or a list of GPU indices and "cpu". This build has no GPU backends yet.
  bool wantCpu = false;
  if (s.devices == "all") {
    wantCpu = true;
    if (s.backend == BackendKind::Auto)
      sel.notes.push_back("backend auto: no GPU backend in this build (Vulkan RT arrives in M5, CUDA in M6); "
                          "rendering on the CPU");
  } else if (s.devices == "gpu") {
    throw NoDeviceError("--devices gpu: this build has no GPU backends yet (Vulkan RT in M5, CUDA in M6)");
  } else {
    size_t start = 0;
    for (;;) {
      const size_t comma = s.devices.find(',', start);
      const std::string item = s.devices.substr(start, comma - start);
      if (item == "cpu") wantCpu = true;
      else throw NoDeviceError(fmt::format("--devices {}: GPU {} is not available in this build", s.devices, item));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  }

  if (wantCpu) {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    uint32_t threads = s.threads;
    if (threads == 0) {
      threads = controllerHost && hw > 2 ? hw - 2 : hw;
      if (controllerHost && hw > 2)
        sel.notes.push_back(fmt::format("cpu: using {} of {} threads, leaving 2 for the controller (--threads to "
                                        "override)",
                                        threads, hw));
    }
    sel.devices.push_back(createCpuBackend(threads));
  }
  if (sel.devices.empty()) throw NoDeviceError("no usable render device");
  return sel;
}

}  // namespace pen
