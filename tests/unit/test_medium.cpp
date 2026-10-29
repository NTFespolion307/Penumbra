#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/render/medium.h"
#include "core/sampling/sampler.h"
#include "core/scene/scene_loader.h"

using namespace pen;
using Catch::Approx;

namespace {

MediumLayer layer(Vec3f sigmaT, float base, float height, Vec3f albedo = Vec3f(1)) {
  MediumLayer l;
  l.sigma_t = sigmaT;
  l.sigma_s = sigmaT * albedo;
  l.base = base;
  l.inv_height = height > 0 ? 1 / height : 0;
  return l;
}

// Midpoint rule over many steps: the reference for the closed form.
double numericDepth(const MediumLayer& l, Vec3f o, Vec3f d, double t) {
  const int n = 200000;
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    const double s = (i + 0.5) * t / n;
    sum += std::exp(-(o.y + s * d.y - l.base) * l.inv_height);
  }
  return sum * t / n;
}

}  // namespace

TEST_CASE("height fog optical depth matches numeric integration; distance inverts it", "[medium]") {
  const MediumLayer l = layer(Vec3f(1), 5, 30);
  const Vec3f o{0, 12, 0};
  for (Vec3f d : {Vec3f{0, 1, 0}, Vec3f{0, -1, 0}, normalize(Vec3f{1, 0.3f, 0}), normalize(Vec3f{1, -0.2f, 1}),
                  Vec3f{1, 0, 0}}) {
    INFO(d.x << " " << d.y << " " << d.z);
    const float depth = layerDepth(l, o, d, 40);
    CHECK(depth == Approx(numericDepth(l, o, d, 40)).epsilon(1e-4));
    CHECK(layerDistance(l, o, d, depth) == Approx(40).epsilon(1e-4));
  }
  // Up: the whole layer above has a finite depth; a larger depth is never reached. Down or level: unbounded.
  const float up = layerDepth(l, o, Vec3f{0, 1, 0}, std::numeric_limits<float>::infinity());
  CHECK(up == Approx(30 * std::exp(-(12.0 - 5) / 30)).epsilon(1e-5));
  CHECK(std::isinf(layerDistance(l, o, Vec3f{0, 1, 0}, up * 1.01f)));
  CHECK(std::isinf(layerDepth(l, o, Vec3f{1, 0, 0}, std::numeric_limits<float>::infinity())));
}

TEST_CASE("transmittance through a uniform medium is the closed form exp(-sigma t)", "[medium][gate]") {
  Medium m;
  m.layers[0] = layer({0.1f, 0.2f, 0.4f}, 0, 0);  // no falloff: uniform
  m.count = 1;
  const Vec3f T = mediumTransmittance(m, {0, 3, 0}, normalize(Vec3f{1, 2, 3}), 7.5f);
  CHECK(T.x == Approx(std::exp(-0.75)).epsilon(1e-6));
  CHECK(T.y == Approx(std::exp(-1.5)).epsilon(1e-6));
  CHECK(T.z == Approx(std::exp(-3.0)).epsilon(1e-6));
}

TEST_CASE("free-flight sampling is unbiased per colour channel", "[medium]") {
  // Two layers with different colours and heights: E[weight] over the no-collision branch is the transmittance
  // in every channel, and over the collision branch it is the scattered fraction (1 - T without absorption).
  Medium m;
  m.layers[0] = layer({0.02f, 0.05f, 0.12f}, 0, 40);
  m.layers[1] = layer(Vec3f(0.08f), -10, 8);
  m.count = 2;
  const Vec3f o{0, 2, 0}, d = normalize(Vec3f{1, 0.15f, 0});
  const float tmax = 25;
  const Vec3f T = mediumTransmittance(m, o, d, tmax);
  Vec3f pass(0), scatter(0);
  const int n = 1 << 18;
  for (int i = 0; i < n; ++i) {
    Sampler s(i & 1023, i >> 10, 0, 7);
    const float uc = s.next1D();
    const float u[3] = {s.next1D(), s.next1D(), s.next1D()};
    const MediumEvent ev = sampleMedium(m, o, d, tmax, uc, u);
    (ev.scattered ? scatter : pass) += ev.weight;
  }
  pass = pass / float(n);
  scatter = scatter / float(n);
  for (int c = 0; c < 3; ++c) {
    INFO("channel " << c);
    CHECK(pass[c] == Approx(T[c]).epsilon(0.01));
    CHECK(scatter[c] == Approx(1 - T[c]).epsilon(0.01));
  }
}

TEST_CASE("phase functions: sampling matches the value; mean cosines", "[medium]") {
  for (const bool rayleigh : {false, true}) {
    MediumLayer l;
    l.rayleigh = rayleigh;
    l.g = 0.7f;
    const Vec3f in = normalize(Vec3f{0.3f, -1, 0.2f});
    double meanCos = 0, meanCos2 = 0;
    const int n = 1 << 16;
    for (int i = 0; i < n; ++i) {
      Sampler s(i, 1, 0, 3);
      float pdf = 0;
      const Vec3f out = samplePhase(l, in, s.next2D(), pdf);
      CHECK(length(out) == Approx(1).epsilon(1e-4));
      CHECK(pdf == Approx(phaseValue(l, in, out)).epsilon(1e-5));
      const double c = dot(in, out);
      meanCos += c / n;
      meanCos2 += c * c / n;
    }
    INFO((rayleigh ? "Rayleigh" : "Henyey-Greenstein"));
    if (rayleigh) {
      CHECK(meanCos == Approx(0).margin(0.01));
      CHECK(meanCos2 == Approx(0.4).epsilon(0.01));  // E[cos^2] of (1 + cos^2) is 2/5
    } else {
      CHECK(meanCos == Approx(0.7).epsilon(0.01));  // the mean cosine of Henyey-Greenstein is g
    }
  }
}

TEST_CASE("the atmosphere's layers from the settings", "[medium]") {
  RenderSettings s;
  CHECK(buildMedium(s).empty());
  s.atmosphere.enabled = true;
  s.atmosphere.altitude = 1000;
  s.atmosphere.meters_per_unit = 2;
  s.atmosphere.fog_density = 0.01;
  const Medium m = buildMedium(s);
  REQUIRE(m.count == 3);  // air, haze, fog
  CHECK(m.layers[0].rayleigh);
  CHECK(m.layers[0].sigma_t.z == Approx(33.1e-6 * 2));  // per scene unit
  CHECK(m.layers[0].base == Approx(-500));               // sea level, in scene units below y = 0
  CHECK(m.layers[1].sigma_t.x == Approx(2e-5 * 1.11 * 2));
  s.atmosphere.air = 0;
  s.atmosphere.haze = 0;
  CHECK(buildMedium(s).count == 1);  // fog only
}
