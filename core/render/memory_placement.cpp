#include "core/render/memory_placement.h"

#include <fmt/format.h>

#include <algorithm>

#include "core/settings/parse.h"

namespace pen {

uint64_t TextureSize::bytes(uint32_t halvings) const {
  uint64_t w = width, h = height;
  for (uint32_t i = 0; i < halvings; ++i) {
    w = std::max<uint64_t>(1, w / 2);
    h = std::max<uint64_t>(1, h / 2);
  }
  return w * h * bytes_per_texel;
}

uint64_t SceneFootprint::textureBytes() const {
  uint64_t total = 0;
  for (const TextureSize& t : textures) total += t.bytes();
  return total;
}

Placement choosePlacement(const SceneFootprint& fp, const MemoryBudgets& b) {
  Placement p;
  const uint64_t movable = fp.environment + fp.textureBytes();
  if (fp.fixed > b.vram) {
    p.tier = 3;
    p.reason = fmt::format("the geometry and acceleration structures need {}, the GPU has {} usable",
                           formatBytes(fp.fixed), formatBytes(b.vram));
    return p;
  }
  if (b.min_tier == 0 && fp.fixed + movable <= b.vram) {
    p.vram_bytes = fp.fixed + movable;
    return p;
  }
  if (b.min_tier <= 1 && movable <= b.ram) {
    p.tier = 1;
    p.vram_bytes = fp.fixed;
    p.ram_bytes = movable;
    p.movable_in_ram = true;
    return p;
  }
  if (b.min_tier >= 3) {
    p.tier = 3;
    p.reason = "it ran out of memory at every tier";
    return p;
  }
  if (!b.allow_downsize) {
    p.tier = 3;
    p.reason = fmt::format("the textures ({}) fit neither in VRAM ({} left) nor in RAM ({} left), and "
                           "--no-texture-downsize forbids smaller textures",
                           formatBytes(movable), formatBytes(b.vram - fp.fixed), formatBytes(b.ram));
    return p;
  }
  // Tier 2: halve the largest texture until everything fits where there is more room. Deterministic, so every
  // device with the same budgets gets the same textures.
  const uint64_t vramLeft = b.vram - fp.fixed;
  const bool inRam = b.ram >= vramLeft;
  const uint64_t room = inRam ? b.ram : vramLeft;
  if (fp.environment > room) {
    p.tier = 3;
    p.reason = fmt::format("the environment map alone needs {}, more than the {} left in {}",
                           formatBytes(fp.environment), formatBytes(room), inRam ? "RAM" : "VRAM");
    return p;
  }
  p.halvings.assign(fp.textures.size(), 0);
  uint64_t total = movable;
  while (total > room) {
    size_t best = fp.textures.size();
    uint64_t bestBytes = 0;
    for (size_t i = 0; i < fp.textures.size(); ++i) {
      const uint64_t now = fp.textures[i].bytes(p.halvings[i]);
      if (now > bestBytes && fp.textures[i].bytes(p.halvings[i] + 1) < now) best = i, bestBytes = now;
    }
    if (best == fp.textures.size()) {
      p.tier = 3;
      p.halvings.clear();
      p.reason = fmt::format("even at 1x1 texels the textures do not fit the {} left in {}", formatBytes(room),
                             inRam ? "RAM" : "VRAM");
      return p;
    }
    total -= bestBytes - fp.textures[best].bytes(p.halvings[best] + 1);
    ++p.halvings[best];
  }
  p.tier = 2;
  p.movable_in_ram = inRam;
  p.vram_bytes = fp.fixed + (inRam ? 0 : total);
  p.ram_bytes = inRam ? total : 0;
  return p;
}

std::string describePlacement(const Placement& p, const MemoryBudgets& b) {
  if (p.tier == 3) return fmt::format("tier 3, skipped: {}", p.reason);
  std::string s = fmt::format("tier {}, scene {} of {} VRAM", p.tier, formatBytes(p.vram_bytes), formatBytes(b.vram));
  if (p.ram_bytes) s += fmt::format(", textures {} in RAM", formatBytes(p.ram_bytes));
  if (p.tier == 2) {
    uint32_t most = 0, count = 0;
    for (uint32_t h : p.halvings) {
      most = std::max(most, h);
      count += h > 0;
    }
    s += fmt::format(", {} texture(s) downsized (at most 1/{} per side)", count, 1u << most);
  }
  return s;
}

}  // namespace pen
