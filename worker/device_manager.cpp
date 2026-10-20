#include "worker/device_manager.h"

#include <algorithm>
#include <fmt/format.h>
#include <stdexcept>
#include <thread>

#include "backends/cpu/cpu_backend.h"
#include "backends/null/null_backend.h"
#include "core/system/sysinfo.h"
#if defined(PEN_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.h"
#endif
#include "worker/test_faults.h"

namespace pen {

namespace {

// The GPUs Penumbra can render on, in --devices numbering (ray-tracing GPUs in Vulkan order), with notes about
// every GPU it skips.
struct GpuList {
  std::vector<uint32_t> physical;  // Vulkan physical device index per Penumbra GPU index
  std::vector<std::string> names;
};

GpuList rayTracingGpus(std::vector<std::string>& notes) {
  GpuList list;
#if defined(PEN_HAS_VULKAN)
  const VulkanProbe& probe = probeVulkan();
  if (!probe.loader_found) {
    notes.push_back(fmt::format("Vulkan: {}", probe.error));
    return list;
  }
  if (!probe.error.empty()) notes.push_back(fmt::format("Vulkan: {}", probe.error));
  for (const VulkanGpu& g : probe.gpus) {
    if (g.ray_tracing) {
      list.physical.push_back(g.physical_index);
      list.names.push_back(g.name);
    } else {
      notes.push_back(fmt::format("skipping {} ({}): no Vulkan ray tracing (missing {})", g.name, g.type, g.missing));
    }
  }
#else
  notes.push_back("this build has no GPU backend (built with PEN_ENABLE_VULKAN=OFF)");
#endif
  return list;
}

}  // namespace

DeviceSelection createDevices(const RenderSettings& s, bool controllerHost) {
  DeviceSelection sel;

  switch (s.backend) {
    case BackendKind::Cuda: throw NoDeviceError("--backend cuda is built in milestone M6; use --backend vulkan or cpu");
    case BackendKind::Optix: throw NoDeviceError("--backend optix is built in milestone M13; use --backend vulkan or cpu");
    default: break;
  }
  const bool nullBackend = s.backend == BackendKind::Null;
  const bool gpuAllowed = s.backend == BackendKind::Auto || s.backend == BackendKind::Vulkan;

  // --devices: "all", "gpu", or a list of GPU indices and "cpu" (repeat "cpu" for several CPU slots, D-046).
  uint32_t cpuSlots = 0;
  std::vector<uint32_t> gpuIndices;
  bool allGpus = false;
  if (s.devices == "all") {
    allGpus = gpuAllowed && !nullBackend;
  } else if (s.devices == "gpu") {
    if (!gpuAllowed) throw NoDeviceError(fmt::format("--devices gpu does not go with --backend {}", toString(s.backend)));
    allGpus = true;
  } else {
    size_t start = 0;
    for (;;) {
      const size_t comma = s.devices.find(',', start);
      const std::string item = s.devices.substr(start, comma - start);
      if (item == "cpu") ++cpuSlots;
      else gpuIndices.push_back(static_cast<uint32_t>(std::stoul(item)));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (!gpuIndices.empty() && !gpuAllowed)
      throw NoDeviceError(fmt::format("--devices {} names GPUs, but --backend {} renders on the CPU", s.devices,
                                      toString(s.backend)));
  }

  // GPUs (docs/GUIDE.md "--backend auto": Vulkan RT on every GPU that has it).
  if (!nullBackend && (allGpus || !gpuIndices.empty())) {
    const GpuList gpus = rayTracingGpus(sel.notes);
    std::vector<uint32_t> wanted = gpuIndices;
    if (allGpus)
      for (uint32_t i = 0; i < gpus.physical.size(); ++i) wanted.push_back(i);
    for (uint32_t i : wanted) {
      if (i >= gpus.physical.size())
        throw NoDeviceError(fmt::format("--devices {}: there is no ray-tracing GPU {} (penumbra --list-devices shows "
                                        "them)",
                                        s.devices, i));
#if defined(PEN_HAS_VULKAN)
      try {
        sel.devices.push_back(createVulkanBackend(gpus.physical[i]));
        sel.notes.push_back(fmt::format("GPU {}: {}, backend vulkan", i, gpus.names[i]));
      } catch (const BackendError& e) {
        if (!allGpus || s.backend == BackendKind::Vulkan) throw;
        sel.notes.push_back(fmt::format("skipping GPU {} ({}): {}", i, gpus.names[i], e.what()));
      }
#endif
    }
    if (sel.devices.empty() && s.backend == BackendKind::Vulkan)
      throw NoDeviceError("--backend vulkan: no GPU with Vulkan ray tracing (penumbra --list-devices shows why)");
    if (sel.devices.empty() && s.devices == "gpu") throw NoDeviceError("--devices gpu: no GPU with Vulkan ray tracing");
    if (sel.devices.empty() && allGpus) {
      cpuSlots = 1;
      sel.notes.push_back("backend auto: no GPU with Vulkan ray tracing; rendering on the CPU");
    }
  } else if (s.devices == "all") {
    cpuSlots = 1;
  }

  if (nullBackend) {
    sel.notes.push_back(fmt::format("backend null (test only): {} device(s) returning noise instantly", cpuSlots));
    for (uint32_t i = 0; i < cpuSlots; ++i) sel.devices.push_back(createNullBackend(i));
  } else if (cpuSlots > 0) {
    const uint32_t hw = usableCpuThreads();
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
