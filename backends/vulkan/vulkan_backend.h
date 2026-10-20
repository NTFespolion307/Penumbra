#pragma once

// Vulkan ray tracing backend (docs/GUIDE.md "Rendering backends"). This header has no Vulkan types, so the
// device manager and --list-devices can use it without including Vulkan.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backends/backend.h"

namespace pen {

struct VulkanGpu {
  uint32_t physical_index = 0;  // in vkEnumeratePhysicalDevices order
  std::string name;
  std::string driver;           // driver name and version
  std::string type;             // "discrete", "integrated", ...
  uint64_t vram_bytes = 0;      // device-local heap size
  bool ray_tracing = false;     // VK_KHR_ray_tracing_pipeline + acceleration structures + the features Penumbra needs
  std::string missing;          // why ray tracing is unusable, when it is
};

struct VulkanProbe {
  bool loader_found = false;  // a Vulkan driver is installed
  std::string error;          // why probing failed, if it did
  std::vector<VulkanGpu> gpus;
};

// Loads Vulkan (volk) and lists the GPUs. Never throws; a machine without Vulkan gives loader_found = false.
const VulkanProbe& probeVulkan();

// A backend on one ray-tracing GPU (VulkanGpu::physical_index). Throws BackendError.
std::unique_ptr<IBackend> createVulkanBackend(uint32_t physicalIndex);

}  // namespace pen
