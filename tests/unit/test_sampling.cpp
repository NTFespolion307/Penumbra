#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <set>
#include <vector>

#include "core/sampling/alias_table.h"
#include "core/sampling/sampler.h"
#include "core/sampling/warp.h"

using namespace pen;

TEST_CASE("sampler is a pure function of pixel, sample, dimension and seed", "[sampling]") {
  Sampler a(10, 20, 5, 7), b(10, 20, 5, 7);
  for (int i = 0; i < 20; ++i) {
    const Vec2f x = a.next2D(), y = b.next2D();
    CHECK(x.x == y.x);
    CHECK(x.y == y.y);
    CHECK(x.x >= 0.0f);
    CHECK(x.x < 1.0f);
    CHECK(x.y >= 0.0f);
    CHECK(x.y < 1.0f);
  }
  Sampler c(11, 20, 5, 7), d(10, 20, 5, 8);
  Sampler e(10, 20, 5, 7);
  const Vec2f ve = e.next2D();
  CHECK(c.next2D().x != ve.x);
  CHECK(d.next2D().x != ve.x);
}

TEST_CASE("jumping to a dimension gives the same numbers as stepping there", "[sampling]") {
  Sampler a(3, 4, 9, 0), b(3, 4, 9, 0);
  for (int i = 0; i < 7; ++i) a.next2D();
  b.setDimension(7);
  const Vec2f x = a.next2D(), y = b.next2D();
  CHECK(x.x == y.x);
  CHECK(x.y == y.y);
}

TEST_CASE("Owen-scrambled Sobol points are stratified per pixel and dimension", "[sampling]") {
  // The first 64 points of a (0,2)-sequence put exactly one point in each cell of an 8x8 grid, and in each
  // cell of 64x1, 32x2, 16x4, ... elementary intervals.
  for (uint32_t dim = 0; dim < 6; ++dim) {
    for (int shape : {0, 3, 6}) {
      const int nx = 1 << shape, ny = 64 >> shape;
      std::set<int> cells;
      for (uint32_t s = 0; s < 64; ++s) {
        Sampler sp(17, 33, s, 1234);
        sp.setDimension(dim);
        const Vec2f u = sp.next2D();
        cells.insert(static_cast<int>(u.x * nx) * ny + static_cast<int>(u.y * ny));
      }
      INFO("dim " << dim << " shape " << nx << "x" << ny);
      CHECK(cells.size() == 64);
    }
  }
}

TEST_CASE("alias table reproduces the weights", "[sampling]") {
  const std::vector<float> w = {1, 0, 3, 6, 0.5f, 9.5f};
  const AliasTable t(w);
  CHECK(t.sum() == 20.0f);
  for (size_t i = 0; i < w.size(); ++i) CHECK(std::fabs(t.pmf(static_cast<uint32_t>(i)) - w[i] / 20) < 1e-7);
  std::vector<int> counts(w.size(), 0);
  constexpr int N = 200000;
  for (int i = 0; i < N; ++i) ++counts[t.sample((i + 0.5f) / N)];
  for (size_t i = 0; i < w.size(); ++i) CHECK(std::fabs(counts[i] / double(N) - w[i] / 20) < 1e-3);
  CHECK(counts[1] == 0);
}

TEST_CASE("alias remap is uniform within the chosen bin", "[sampling]") {
  const AliasTable t(std::vector<float>{1, 3});
  double sum = 0;
  int n = 0;
  constexpr int N = 100000;
  for (int i = 0; i < N; ++i) {
    float r = 0;
    if (t.sample((i + 0.5f) / N, &r) == 1) {
      sum += r;
      ++n;
    }
    REQUIRE(r >= 0.0f);
    REQUIRE(r < 1.0f);
  }
  CHECK(std::fabs(sum / n - 0.5) < 2e-3);
}

TEST_CASE("2D distribution pdf integrates to one and matches sampling", "[sampling]") {
  std::vector<float> w(16 * 8);
  for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>((i * 7) % 5);
  const Distribution2D d(w, 16, 8);
  double integral = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x) integral += d.pdf({(x + 0.5f) / 16, (y + 0.5f) / 8}) / (16 * 8);
  CHECK(std::fabs(integral - 1) < 1e-5);
  for (int i = 0; i < 1000; ++i) {
    float pdf = 0;
    const Vec2f p = d.sample({(i % 37 + 0.5f) / 37, (i / 37 + 0.5f) / 28}, pdf);
    CHECK(pdf > 0);
    CHECK(std::fabs(pdf - d.pdf(p)) < 1e-4f * pdf);
  }
}

TEST_CASE("warps produce unit vectors with the right densities", "[sampling]") {
  for (int i = 0; i < 100; ++i) {
    const Vec2f u{(i % 10 + 0.5f) / 10, (i / 10 + 0.5f) / 10};
    const Vec3f c = sampleCosineHemisphere(u);
    CHECK(std::fabs(length(c) - 1) < 1e-5f);
    CHECK(c.z >= 0);
    CHECK(std::fabs(length(sampleUniformSphere(u)) - 1) < 1e-5f);
    const Vec2f b = sampleUniformTriangle(u);
    CHECK(b.x >= 0);
    CHECK(b.y >= 0);
    CHECK(b.x + b.y <= 1.00001f);
  }
  CHECK(powerHeuristic(1, 1) == 0.5f);
  CHECK(powerHeuristic(1, 0) == 1.0f);
}
