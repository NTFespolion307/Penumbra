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

// A GPU Penumbra can render on, numbered as --devices takes it: GPUs with Vulkan ray tracing in Vulkan order, then
// NVIDIA GPUs only the CUDA backend can use, in CUDA order (DECISIONS D-088). A GPU on both lists (matched by
// UUID) is one entry with both backends.
struct RenderGpu {
  uint32_t index = 0;
  std::string name;
  uint64_t vram_bytes = 0;
  int vulkan = -1;  // Vulkan physical device index, when it has ray tracing
  int cuda = -1;    // CUDA ordinal, when this build's kernels run on it
};

struct GpuInventory {
  std::vector<RenderGpu> gpus;
  std::vector<std::string> notes;  // GPUs left out, and why; missing drivers
};

// Probes Vulkan and CUDA once (cached). Never throws.
const GpuInventory& gpuInventory();

// Enumerates this machine's devices and creates one backend per selected device. On the controller host the
// CPU device leaves 2 threads free when --threads is not given (docs/GUIDE.md "Controller and workers").
DeviceSelection createDevices(const RenderSettings& settings, bool controllerHost);

}  // namespace pen
