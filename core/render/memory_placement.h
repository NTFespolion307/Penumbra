#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pen {

// The size of one texture as a device stores it.
struct TextureSize {
  uint32_t width = 0, height = 0;
  uint32_t bytes_per_texel = 4;  // RGBA8 4, float RGBA 16
  // Bytes after halving the texture `halvings` times (each side max(1, n / 2), like a mip level).
  uint64_t bytes(uint32_t halvings = 0) const;
};

// What one device needs to hold a scene (docs/GUIDE.md "Memory budget and fallback"), in bytes.
struct SceneFootprint {
  uint64_t fixed = 0;                 // must be in VRAM: geometry, acceleration structures, materials, lights, tables
  uint64_t environment = 0;           // the HDRI map and its sampling weights: VRAM or RAM, never downsized
  std::vector<TextureSize> textures;  // each texture at full size: VRAM or RAM, downsized at tier 2
  uint64_t textureBytes() const;
};

struct MemoryBudgets {
  uint64_t vram = 0;  // usable VRAM: free VRAM minus 10%, or --vram-limit when smaller
  uint64_t ram = 0;   // pinned RAM this host still has for this device
  bool allow_downsize = true;  // false: --no-texture-downsize
  uint32_t min_tier = 0;       // after an allocation failed at a tier, the next try starts one lower on the ladder
};

// Where a device keeps the scene: the tier ladder of docs/GUIDE.md.
//   0  everything in VRAM
//   1  textures and the environment in pinned RAM, read over PCIe; geometry in VRAM
//   2  as 1 (or in VRAM, whichever has more room), with the largest textures halved until they fit
//   3  the geometry does not fit (or the textures nowhere, downsizing forbidden): the device sits this render out
struct Placement {
  uint32_t tier = 0;
  uint64_t vram_bytes = 0, ram_bytes = 0;  // what the device will use
  bool movable_in_ram = false;             // textures and environment live in pinned RAM
  std::vector<uint32_t> halvings;          // tier 2: times each texture is halved (empty, or one entry per texture)
  std::string reason;                      // tier 3: why
};

Placement choosePlacement(const SceneFootprint& footprint, const MemoryBudgets& budgets);

// "tier 1, scene 6.1 GB of 7.2 GB VRAM, textures 2.4 GB in RAM" (the placement log, docs/GUIDE.md).
std::string describePlacement(const Placement& placement, const MemoryBudgets& budgets);

}  // namespace pen
