#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "core/settings/presets.h"

using namespace pen;

TEST_CASE("the 17 resolution presets match the guide", "[presets]") {
  const auto presets = resolutionPresets();
  REQUIRE(presets.size() == 17);

  // Guide table: preset, width, height, megapixels (rounded as printed there).
  struct Row {
    const char* name;
    uint32_t w, h;
    double mp;
  };
  const Row guide[] = {
      {"120p", 214, 120, 0.03},       {"240p", 426, 240, 0.10},      {"360p", 640, 360, 0.23},
      {"480p", 854, 480, 0.41},       {"720p", 1280, 720, 0.92},     {"1080p", 1920, 1080, 2.07},
      {"2k", 2560, 1440, 3.69},       {"4k", 3840, 2160, 8.29},      {"6k", 5760, 3240, 18.7},
      {"8k", 7680, 4320, 33.2},       {"10k", 10240, 5760, 59.0},    {"15k", 15360, 8640, 133},
      {"20k", 20480, 11520, 236},     {"40k", 40960, 23040, 944},    {"60k", 61440, 34560, 2123},
      {"80k", 81920, 46080, 3775},    {"160k", 163840, 92160, 15099},
  };
  for (size_t i = 0; i < presets.size(); ++i) {
    INFO(guide[i].name);
    CHECK(presets[i].name == guide[i].name);
    CHECK(presets[i].width == guide[i].w);
    CHECK(presets[i].height == guide[i].h);
    CHECK(presets[i].width % 2 == 0);
    CHECK(presets[i].height % 2 == 0);
    const double mp = double(presets[i].width) * presets[i].height / 1e6;
    CHECK(std::abs(mp - guide[i].mp) <= std::max(0.005, guide[i].mp * 0.005));  // guide values are rounded
  }
}

// The guide says "from 6K up"; 6k (5760x3240) is 4.5x 1280x720, so the rule holds from 8k (DECISIONS.md).
TEST_CASE("presets from 8k up are multiples of 1280x720 and 16:9", "[presets]") {
  bool from8k = false;
  for (const auto& p : resolutionPresets()) {
    if (p.name == "8k") from8k = true;
    if (!from8k) continue;
    INFO(std::string(p.name));
    CHECK(p.width % 1280 == 0);
    CHECK(p.height % 720 == 0);
    CHECK(p.width * 9 == p.height * 16);
  }
}

TEST_CASE("160k film is about 241.6 GB at 16 bytes per pixel", "[presets]") {
  const ResolutionPreset* p = findResolutionPreset("160K");
  REQUIRE(p);
  const double gb = double(p->width) * p->height * 16 / 1e9;
  CHECK(std::abs(gb - 241.6) < 0.1);
}

TEST_CASE("preset names are unique and lookup is case-insensitive", "[presets]") {
  std::set<std::string> names;
  for (const auto& p : resolutionPresets()) names.insert(std::string(p.name));
  CHECK(names.size() == 17);
  CHECK(findResolutionPreset("1080P") == findResolutionPreset("1080p"));
  CHECK(findResolutionPreset("16k") == nullptr);
}
