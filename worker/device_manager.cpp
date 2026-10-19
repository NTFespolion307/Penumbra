#include "worker/device_manager.h"

#include <algorithm>
#include <fmt/format.h>
#include <stdexcept>
#include <thread>

#include "backends/cpu/cpu_backend.h"
#include "backends/null/null_backend.h"
#include "worker/test_faults.h"

namespace pen {

DeviceSelection createDevices(const RenderSettings& s, bool controllerHost) {
  DeviceSelection sel;

  switch (s.backend) {
    case BackendKind::Vulkan: throw NoDeviceError("--backend vulkan is built in milestone M5; use --backend cpu");
    case BackendKind::Cuda: throw NoDeviceError("--backend cuda is built in milestone M6; use --backend cpu");
    case BackendKind::Optix: throw NoDeviceError("--backend optix is built in milestone M13; use --backend cpu");
    default: break;
  }

  // --devices: "all", "gpu", or a list of GPU indices and "cpu" (repeat "cpu" for several CPU slots, D-046).
  uint32_t cpuSlots = 0;
  if (s.devices == "all") {
    cpuSlots = 1;
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
      if (item == "cpu") ++cpuSlots;
      else throw NoDeviceError(fmt::format("--devices {}: GPU {} is not available in this build", s.devices, item));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  }

  if (s.backend == BackendKind::Null) {
    sel.notes.push_back(fmt::format("backend null (test only): {} device(s) returning noise instantly", cpuSlots));
    for (uint32_t i = 0; i < cpuSlots; ++i) sel.devices.push_back(createNullBackend(i));
  } else if (cpuSlots > 0) {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    uint32_t threads = s.threads;
    if (threads == 0) {
      threads = controllerHost && hw > 2 ? hw - 2 : hw;
      if (controllerHost && hw > 2)
        sel.notes.push_back(fmt::format("cpu: using {} of {} threads, leaving 2 for the controller (--threads to "
                                        "override)",
                                        threads, hw));
    }
    const uint32_t perSlot = std::max(1u, threads / cpuSlots);
    if (cpuSlots > 1)
      sel.notes.push_back(fmt::format("cpu: {} slots with {} threads each", cpuSlots, perSlot));
    for (uint32_t i = 0; i < cpuSlots; ++i) sel.devices.push_back(createCpuBackend(perSlot));
  }
  if (sel.devices.empty()) throw NoDeviceError("no usable render device");

  std::optional<FaultSpec> fault;
  try {
    fault = faultSpecFromEnvironment();
  } catch (const std::invalid_argument& e) {
    throw NoDeviceError(e.what());
  }
  if (fault) {
    if (fault->device >= sel.devices.size())
      throw NoDeviceError(fmt::format("PENUMBRA_TEST_FAULTS: there is no device {}", fault->device));
    sel.notes.push_back(fmt::format("TEST FAULT INJECTION ACTIVE (PENUMBRA_TEST_FAULTS): device {} will {} after {} "
                                    "item(s)",
                                    fault->device, toString(fault->mode), fault->after));
    sel.devices[fault->device] = wrapWithFault(std::move(sel.devices[fault->device]), *fault);
  }
  return sel;
}

}  // namespace pen
