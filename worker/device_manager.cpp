#include "worker/device_manager.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <stdexcept>
#include <thread>

#include "backends/cpu/cpu_backend.h"
#include "backends/null/null_backend.h"
#include "core/system/sysinfo.h"
#if defined(PEN_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.h"
#endif
#if defined(PEN_HAS_CUDA)
#include "backends/cuda/cuda_backend.h"
#endif
#include "worker/test_faults.h"

namespace pen {

const GpuInventory& gpuInventory() {
  static const GpuInventory inv = [] {
    GpuInventory out;
    std::vector<std::string> noRt;  // Vulkan GPUs without ray tracing, unless CUDA reports them
    std::vector<std::array<uint8_t, 16>> noRtUuid, rtUuid;
#if defined(PEN_HAS_VULKAN)
    const VulkanProbe& vk = probeVulkan();
    if (!vk.error.empty()) out.notes.push_back(fmt::format("Vulkan: {}", vk.error));
    for (const VulkanGpu& g : vk.gpus) {
      if (g.ray_tracing) {
        RenderGpu r;
        r.index = static_cast<uint32_t>(out.gpus.size());
        r.name = g.name;
        r.vram_bytes = g.vram_bytes;
        r.vulkan = static_cast<int>(g.physical_index);
        out.gpus.push_back(r);
        rtUuid.push_back(g.uuid);
      } else {
        noRt.push_back(fmt::format("{} ({}): no Vulkan ray tracing (missing {})", g.name, g.type, g.missing));
        noRtUuid.push_back(g.uuid);
      }
    }
#else
    out.notes.push_back("Vulkan: not in this build (PEN_ENABLE_VULKAN=OFF)");
#endif
#if defined(PEN_HAS_CUDA)
    const CudaProbe& cu = probeCuda();
    if (!cu.driver_found) out.notes.push_back(fmt::format("CUDA: {}", cu.error));
    const std::array<uint8_t, 16> zero{};
    for (const CudaGpu& g : cu.gpus) {
      int match = -1;
      for (size_t i = 0; i < rtUuid.size(); ++i)
        if (g.uuid != zero && rtUuid[i] == g.uuid) match = static_cast<int>(i);
      for (size_t i = 0; i < noRtUuid.size(); ++i)
        if (g.uuid != zero && noRtUuid[i] == g.uuid) noRt[i].clear();  // CUDA's verdict covers it
      if (!g.usable) {
        out.notes.push_back(fmt::format("not used: {}: {}", g.name, g.why));
        continue;
      }
      if (match >= 0) {
        out.gpus[static_cast<size_t>(match)].cuda = static_cast<int>(g.ordinal);
        continue;
      }
      RenderGpu r;
      r.index = static_cast<uint32_t>(out.gpus.size());
      r.name = g.name;
      r.vram_bytes = g.vram_bytes;
      r.cuda = static_cast<int>(g.ordinal);
      out.gpus.push_back(r);
    }
#else
    out.notes.push_back("CUDA: not in this build (no CUDA toolkit when it was built, or PEN_ENABLE_CUDA=OFF)");
#endif
    for (const std::string& n : noRt)
      if (!n.empty()) out.notes.push_back(fmt::format("not used: {}", n));
    return out;
  }();
  return inv;
}

namespace {

std::unique_ptr<IBackend> createGpuBackend(const RenderGpu& g, BackendKind kind) {
#if defined(PEN_HAS_VULKAN)
  if (kind == BackendKind::Vulkan) return createVulkanBackend(static_cast<uint32_t>(g.vulkan));
#endif
#if defined(PEN_HAS_CUDA)
  if (kind == BackendKind::Cuda) return createCudaBackend(static_cast<uint32_t>(g.cuda));
#endif
  (void)g;
  throw BackendError(fmt::format("backend {} is not in this build", toString(kind)));
}

}  // namespace

DeviceSelection createDevices(const RenderSettings& s, bool controllerHost) {
  DeviceSelection sel;

  if (s.backend == BackendKind::Optix)
    throw NoDeviceError("--backend optix is built in milestone M13; use --backend vulkan, cuda or cpu");
  const bool nullBackend = s.backend == BackendKind::Null;
  const bool gpuAllowed =
      s.backend == BackendKind::Auto || s.backend == BackendKind::Vulkan || s.backend == BackendKind::Cuda;

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

  // GPUs (docs/GUIDE.md "--backend auto": Vulkan RT where the GPU has it, else CUDA on NVIDIA).
  if (!nullBackend && (allGpus || !gpuIndices.empty())) {
    const GpuInventory& inv = gpuInventory();
    for (const std::string& n : inv.notes) sel.notes.push_back(n);
    std::vector<uint32_t> wanted = gpuIndices;
    if (allGpus)
      for (uint32_t i = 0; i < inv.gpus.size(); ++i) wanted.push_back(i);
    const std::string need = s.backend == BackendKind::Vulkan ? "Vulkan ray tracing"
                             : s.backend == BackendKind::Cuda ? "CUDA"
                                                              : "Vulkan ray tracing or CUDA";
    for (uint32_t i : wanted) {
      if (i >= inv.gpus.size())
        throw NoDeviceError(fmt::format("--devices {}: there is no GPU {} that can render (penumbra --list-devices "
                                        "shows them)",
                                        s.devices, i));
      const RenderGpu& g = inv.gpus[i];
      BackendKind kind = s.backend;
      if (kind == BackendKind::Auto) kind = g.vulkan >= 0 ? BackendKind::Vulkan : BackendKind::Cuda;
      if ((kind == BackendKind::Vulkan && g.vulkan < 0) || (kind == BackendKind::Cuda && g.cuda < 0)) {
        if (!allGpus) throw NoDeviceError(fmt::format("--devices {}: GPU {} ({}) has no {}", s.devices, i, g.name, need));
        sel.notes.push_back(fmt::format("skipping GPU {} ({}): no {}", i, g.name, need));
        continue;
      }
      try {
        sel.devices.push_back(createGpuBackend(g, kind));
        sel.notes.push_back(fmt::format("GPU {}: {}, backend {}{}", i, g.name, toString(kind),
                                        s.backend == BackendKind::Auto && kind == BackendKind::Cuda
                                            ? " (no Vulkan ray tracing: software BVH)"
                                            : ""));
      } catch (const BackendError& e) {
        if (!allGpus || s.backend != BackendKind::Auto) throw;
        sel.notes.push_back(fmt::format("skipping GPU {} ({}): {}", i, g.name, e.what()));
      }
    }
    if (sel.devices.empty() && s.backend != BackendKind::Auto)
      throw NoDeviceError(fmt::format("--backend {}: no GPU with {} (penumbra --list-devices shows why)",
                                      toString(s.backend), need));
    if (sel.devices.empty() && s.devices == "gpu") throw NoDeviceError(fmt::format("--devices gpu: no GPU with {}", need));
    if (sel.devices.empty() && allGpus) {
      cpuSlots = 1;
      sel.notes.push_back(fmt::format("backend auto: no GPU with {}; rendering on the CPU", need));
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
