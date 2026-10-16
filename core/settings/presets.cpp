#include "core/settings/presets.h"

#include <array>

#include "core/settings/parse.h"

namespace pen {

namespace {
constexpr std::array<ResolutionPreset, 17> kPresets{{
    {"120p", 214, 120},
    {"240p", 426, 240},
    {"360p", 640, 360},
    {"480p", 854, 480},
    {"720p", 1280, 720},
    {"1080p", 1920, 1080},
    {"2k", 2560, 1440},
    {"4k", 3840, 2160},
    {"6k", 5760, 3240},
    {"8k", 7680, 4320},
    {"10k", 10240, 5760},
    {"15k", 15360, 8640},
    {"20k", 20480, 11520},
    {"40k", 40960, 23040},
    {"60k", 61440, 34560},
    {"80k", 81920, 46080},
    {"160k", 163840, 92160},
}};
}  // namespace

std::span<const ResolutionPreset> resolutionPresets() { return kPresets; }

const ResolutionPreset* findResolutionPreset(std::string_view name) {
  const std::string lower = toLower(name);
  for (const auto& p : kPresets)
    if (p.name == lower) return &p;
  return nullptr;
}

}  // namespace pen
