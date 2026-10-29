#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/scene/sky.h"

using namespace pen;
using Catch::Approx;

namespace {

constexpr float kPiF = 3.14159265f;

// The map pixel holding direction d (the environment layout, core/render/lights.cpp).
Vec3f lookup(const ImageRGB& map, Vec3f d) {
  float u = std::atan2(-d.z, -d.x) / (2 * kPiF) + 0.5f;
  u -= std::floor(u);
  const float v = std::acos(std::fmax(-1.0f, std::fmin(1.0f, d.y))) / kPiF;
  return map.at(std::min(uint32_t(u * map.width), map.width - 1), std::min(uint32_t(v * map.height), map.height - 1));
}

Vec3f dir(float elevationDeg, float azimuthDeg) {
  const float e = elevationDeg * kPiF / 180, a = azimuthDeg * kPiF / 180;
  return {std::sin(a) * std::cos(e), std::sin(e), -std::cos(a) * std::cos(e)};
}

}  // namespace

TEST_CASE("the sun is where the settings put it", "[sky]") {
  SkySettings sky;
  sky.enabled = true;
  sky.resolution = 1024;
  for (const auto& [elev, azim] : {std::pair{30.0, 0.0}, std::pair{12.0, 135.0}}) {
    sky.sun_elevation_deg = elev;
    sky.sun_azimuth_deg = azim;
    const ImageRGB map = bakeSky(sky);
    const Vec3f s = sunDirection(sky);
    CHECK(s.y == Approx(std::sin(elev * kPiF / 180)).margin(1e-5));
    // The brightest pixel of the map is the sun's.
    uint32_t bx = 0, by = 0;
    float best = -1;
    for (uint32_t y = 0; y < map.height; ++y)
      for (uint32_t x = 0; x < map.width; ++x)
        if (luminance(map.at(x, y)) > best) best = luminance(map.at(x, y)), bx = x, by = y;
    const Vec3f at = lookup(map, s);
    CHECK(luminance(at) > 0.5f * best);
    const float elevFound = 90 - (by + 0.5f) / map.height * 180;
    CHECK(elevFound == Approx(elev).margin(0.4));
  }
}

TEST_CASE("the sky matches Blender's Nishita sky away from the horizon (sun 30 deg)", "[sky][gate]") {
  // Reference values from Blender 4.2.23 (Cycles, Nishita, altitude 1 m, air/dust/ozone 1), D-120. Within 10 deg
  // of the horizon Blender is darker: its view ray adds each step's whole depth before using it.
  SkySettings sky;
  sky.enabled = true;
  sky.sun_elevation_deg = 30;
  sky.resolution = 1024;
  const ImageRGB map = bakeSky(sky);
  const auto close = [](Vec3f ours, Vec3f blender) {
    for (int c = 0; c < 3; ++c) {
      INFO("channel " << c << ": ours " << ours[c] << ", Blender " << blender[c]);
      CHECK(ours[c] == Approx(blender[c]).epsilon(0.08));
    }
  };
  close(lookup(map, dir(89.9f, 0)), {0.7482f, 1.163f, 2.042f});     // zenith
  close(lookup(map, dir(20, 180)), {2.023f, 3.184f, 4.998f});        // away from the sun
  close(lookup(map, dir(50, 0)), {4.283f, 4.568f, 5.613f});          // towards the sun, above it
}

TEST_CASE("the sun disc carries the sun's irradiance at any map resolution", "[sky]") {
  // Integrates (with disc - without) over the map: luminance x solid angle.
  const auto sunPower = [](uint32_t resolution) {
    SkySettings sky;
    sky.enabled = true;
    sky.sun_elevation_deg = 60;
    sky.resolution = resolution;
    const ImageRGB withSun = bakeSky(sky);
    sky.sun_disc = false;
    const ImageRGB without = bakeSky(sky);
    double power = 0;
    for (uint32_t y = 0; y < withSun.height / 2; ++y) {
      const double theta = (y + 0.5) / withSun.height * 3.14159265358979;
      const double dOmega =
          (2 * 3.14159265358979 / withSun.width) * (3.14159265358979 / withSun.height) * std::sin(theta);
      for (uint32_t x = 0; x < withSun.width; ++x) power += luminance(withSun.at(x, y) - without.at(x, y)) * dOmega;
    }
    return power;
  };
  const double coarse = sunPower(2048), fine = sunPower(4096);
  CHECK(fine == Approx(coarse).epsilon(0.02));
  // A 5778 K sun (1361 W/m^2) has Y = 187 above the air; through the air at 60 degrees about 85% arrives.
  CHECK(fine > 140);
  CHECK(fine < 185);
}
