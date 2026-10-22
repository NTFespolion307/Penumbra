#pragma once

// Live GPU readings from NVIDIA's management library (NVML), for Stats and --list-devices (docs/GUIDE.md
// "Stats"; DECISIONS D-093). The library ships with the NVIDIA driver and is loaded at run time, so builds and
// machines without it work the same, just without these readings.

#include <array>
#include <cstdint>
#include <optional>

namespace pen {

struct GpuLiveStats {
  float usage_percent = 0;         // share of the last sample period a kernel ran
  uint64_t vram_used = 0, vram_total = 0;
  float temperature_c = -300.f;    // < -273 when unknown
};

// Readings for the GPU with this UUID (Vulkan deviceUUID / CUDA UUID); nullopt without NVML, for a non-NVIDIA
// GPU, or for an all-zero UUID. Never throws; thread-safe.
std::optional<GpuLiveStats> queryGpuLiveStats(const std::array<uint8_t, 16>& uuid);

}  // namespace pen
