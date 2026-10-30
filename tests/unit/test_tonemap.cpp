#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/output/tonemap.h"

using namespace pen;

namespace {

struct Error {
  double mean = 0, max = 0;
};

// Display values against Blender's view transforms for the same linear inputs (tests/data/tonemap_blender.json,
// written by Blender 4.2 from 217 colours: a grey ramp from -10 to +8 EV and 12 hues at 3 saturations, 5 EVs).
Error against(const char* key, Tonemap mode, bool greyOnly) {
  std::ifstream in(std::string(PEN_TEST_DATA_DIR) + "/tonemap_blender.json");
  const nlohmann::json j = nlohmann::json::parse(in);
  Error e;
  int n = 0;
  for (const auto& r : j["rows"]) {
    const Vec3f v{r["in"][0].get<float>(), r["in"][1].get<float>(), r["in"][2].get<float>()};
    if (greyOnly && !(v.x == v.y && v.y == v.z)) continue;
    const Vec3f ours = tonemap(mode, v);
    for (int c = 0; c < 3; ++c) {
      const double d = std::fabs(ours[c] - r[key][c].get<double>());
      e.mean += d;
      e.max = std::max(e.max, d);
      ++n;
    }
  }
  e.mean /= n;
  return e;
}

}  // namespace

TEST_CASE("tonemaps against Blender's view transforms", "[tonemap][gate]") {
  const Error agx = against("agx", Tonemap::Agx, false);
  const Error filmicGrey = against("filmic", Tonemap::Filmic, true);
  const Error filmic = against("filmic", Tonemap::Filmic, false);
  const Error standard = against("standard", Tonemap::None, false);
  const Error agxGrey = against("agx", Tonemap::Agx, true);
  INFO("AgX: grey max " << agxGrey.max * 255 << ", mean " << agx.mean * 255 << ", max " << agx.max * 255
                        << " (8-bit steps)");
  INFO("Filmic: grey max " << filmicGrey.max * 255 << ", all colours mean " << filmic.mean * 255);
  // Measured 2026-10-30 (D-126): AgX grey within a step, all colours mean 1.4 steps, max 35 (saturated primaries,
  // where Blender's inset or outset differs from the published matrices); Filmic grey exact, colours mean 2.5
  // (Blender also desaturates bright colours); None equals Blender's Standard.
  CHECK(agxGrey.max * 255 < 1.0);
  CHECK(agx.mean * 255 < 2.0);
  CHECK(agx.max * 255 < 40.0);
  CHECK(filmicGrey.max * 255 < 1.0);
  CHECK(filmic.mean * 255 < 3.0);
  CHECK(standard.max * 255 < 1.0);
}

TEST_CASE("ACES and the display range", "[tonemap]") {
  for (const Tonemap m : {Tonemap::Agx, Tonemap::Filmic, Tonemap::Aces, Tonemap::None}) {
    float prev = -1;
    for (int ev = -12; ev <= 12; ++ev) {  // grey: monotone, in [0, 1]
      const Vec3f d = tonemap(m, Vec3f(0.18f * std::exp2(float(ev))));
      CHECK(d.x >= 0);
      CHECK(d.x <= 1);
      CHECK(d.x >= prev);
      prev = d.x;
    }
  }
  // Hill's fit maps 18% grey to about 0.45 after the sRGB curve, and a very bright value to white.
  CHECK(tonemap(Tonemap::Aces, Vec3f(0.18f)).y > 0.35f);
  CHECK(tonemap(Tonemap::Aces, Vec3f(0.18f)).y < 0.55f);
  CHECK(tonemap(Tonemap::Aces, Vec3f(100.0f)).y > 0.99f);
}
