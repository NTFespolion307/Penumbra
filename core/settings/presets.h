#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace pen {

struct ResolutionPreset {
  std::string_view name;  // --res value, e.g. "4k"
  uint32_t width;
  uint32_t height;
};

// The 17 presets from docs/GUIDE.md "Resolution presets and gigapixel output", smallest first.
std::span<const ResolutionPreset> resolutionPresets();

// Case-insensitive lookup ("4K" == "4k"); nullptr if unknown.
const ResolutionPreset* findResolutionPreset(std::string_view name);

inline constexpr uint32_t kMinImageSide = 16;
inline constexpr uint32_t kMaxImageSide = 163840;

}  // namespace pen
