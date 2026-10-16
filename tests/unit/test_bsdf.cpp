#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/sampling/sampler.h"
#include "core/sampling/warp.h"
#include "core/shading/bsdf.h"

using namespace pen;

namespace {

Vec3f dirFromAngle(float thetaDeg, float phiDeg = 30) {
  const float t = thetaDeg * kPi / 180, p = phiDeg * kPi / 180;
  return {std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t)};
}

struct Albedo {
  Vec3f mean;
  int failed = 0;
};

// Monte Carlo directional albedo from BSDF sampling: E[weight].
Albedo albedo(const BsdfInputs& in, Vec3f wo, int n = 1 << 16) {
  const PrincipledBsdf bsdf(in, wo);
  Vec3f sum(0);
  Albedo a;
  for (int i = 0; i < n; ++i) {
    Sampler s(1, 2, static_cast<uint32_t>(i), 99);
    const float uLobe = s.next1D();
    const Vec2f u = s.next2D();
    BsdfSample bs;
    if (bsdf.sample(wo, uLobe, u, bs)) sum += bs.weight;
    else ++a.failed;
  }
  a.mean = sum / static_cast<float>(n);
  return a;
}

std::vector<BsdfInputs> materials() {
  std::vector<BsdfInputs> m;
  BsdfInputs b;
  b.base_color = {0.9f, 0.9f, 0.9f};
  for (float rough : {0.05f, 0.3f, 0.7f, 1.0f}) {
    b.roughness = rough;
    b.metallic = 0;
    m.push_back(b);  // plastic
    b.metallic = 1;
    m.push_back(b);  // metal
    b.metallic = 0;
    b.transmission = 1;
    m.push_back(b);  // rough glass
    b.transmission = 0;
    b.clearcoat = 1;
    b.clearcoat_roughness = 0.1f;
    m.push_back(b);  // coated
    b.clearcoat = 0;
    b.sheen_color = {1, 1, 1};
    b.sheen_roughness = 0.5f;
    m.push_back(b);  // sheen
    b.sheen_color = {0, 0, 0};
    b.anisotropy = 0.8f;
    b.metallic = 1;
    m.push_back(b);  // anisotropic metal
    b.anisotropy = 0;
  }
  return m;
}

}  // namespace

TEST_CASE("a white Lambert surface reflects exactly all energy", "[bsdf]") {
  BsdfInputs in;
  in.base_color = {1, 1, 1};
  in.specular = 0;  // no specular layer: pure Lambert, as in the furnace scene
  in.metallic = 0;
  for (float theta : {0.0f, 30.0f, 60.0f, 85.0f}) {
    const Vec3f wo = dirFromAngle(theta);
    const PrincipledBsdf bsdf(in, wo);
    for (int i = 0; i < 64; ++i) {
      BsdfSample bs;
      REQUIRE(bsdf.sample(wo, 0.5f, {(i % 8 + 0.5f) / 8, (i / 8 + 0.5f) / 8}, bs));
      CHECK(std::fabs(bs.weight.x - 1) < 1e-5f);
    }
  }
}

TEST_CASE("smooth glass conserves energy at every angle", "[bsdf]") {
  BsdfInputs in;
  in.base_color = {1, 1, 1};
  in.metallic = 0;
  in.roughness = 0;
  in.transmission = 1;
  for (float theta : {0.0f, 45.0f, 80.0f}) {
    // Outside: reflection weight 1, refraction weight 1/eta^2 (radiance scaling); inside: refraction eta^2.
    const Vec3f wo = dirFromAngle(theta);
    const PrincipledBsdf out(in, wo);
    const float F = fresnelDielectric(wo.z, 1.5f);
    BsdfSample r, t;
    REQUIRE(out.sample(wo, F * 0.5f, {0.5f, 0.5f}, r));
    REQUIRE(out.sample(wo, F + (1 - F) * 0.5f, {0.5f, 0.5f}, t));
    CHECK(r.delta);
    CHECK(t.delta);
    CHECK(std::fabs(r.weight.x - 1) < 1e-4f);
    CHECK(std::fabs(t.weight.x * 1.5f * 1.5f - 1) < 1e-4f);
    CHECK(t.wi.z < 0);
  }
}

TEST_CASE("no material reflects more energy than it receives", "[bsdf]") {
  for (const BsdfInputs& in : materials()) {
    for (float theta : {5.0f, 45.0f, 75.0f}) {
      const Albedo a = albedo(in, dirFromAngle(theta));
      INFO("rough " << in.roughness << " metal " << in.metallic << " trans " << in.transmission << " coat "
                    << in.clearcoat << " sheen " << in.sheen_color.x << " aniso " << in.anisotropy << " theta "
                    << theta << " albedo " << a.mean.x);
      // Transmission weights carry 1/eta^2 radiance scaling, so compare luminance after undoing it.
      const float scale = in.transmission > 0 ? 1.5f * 1.5f : 1.0f;
      CHECK(maxComponent(a.mean) <= 1.0f * scale + 0.01f);
      CHECK(maxComponent(a.mean) > 0.0f);
    }
  }
}

TEST_CASE("sample() and eval() agree on value and pdf", "[bsdf]") {
  for (BsdfInputs in : materials()) {
    if (in.roughness < 0.1f) continue;  // near-delta lobes are numerically touchy; covered by the albedo test
    const Vec3f wo = dirFromAngle(40);
    const PrincipledBsdf bsdf(in, wo);
    int checked = 0;
    for (int i = 0; i < 256; ++i) {
      Sampler s(4, 5, static_cast<uint32_t>(i), 3);
      const float uLobe = s.next1D();
      const Vec2f u = s.next2D();
      BsdfSample bs;
      if (!bsdf.sample(wo, uLobe, u, bs) || bs.delta) continue;
      float pdf = 0;
      const Vec3f f = bsdf.eval(wo, bs.wi, pdf);
      INFO("rough " << in.roughness << " metal " << in.metallic << " trans " << in.transmission);
      CHECK(std::fabs(pdf - bs.pdf) <= 1e-3f * std::fmax(pdf, 1.0f));
      CHECK(std::fabs(f.x / pdf - bs.weight.x) <= 1e-3f * std::fmax(bs.weight.x, 1.0f));
      ++checked;
    }
    CHECK(checked > 100);
  }
}

TEST_CASE("the pdf integrates to the probability that sampling succeeds", "[bsdf]") {
  // sample() rejects directions that end up on the wrong side (rough GGX reflects part of its microfacet
  // normals below the horizon), so the pdf over valid directions integrates to the success rate, not to 1.
  for (BsdfInputs in : materials()) {
    if (in.roughness < 0.25f || in.clearcoat > 0) continue;  // peaked coat lobes need far more samples
    const Vec3f wo = dirFromAngle(35);
    const PrincipledBsdf bsdf(in, wo);
    constexpr int kN = 1 << 18;
    double integral = 0;
    int succeeded = 0;
    for (int i = 0; i < kN; ++i) {
      Sampler s(8, 9, static_cast<uint32_t>(i), 5);
      const Vec3f wi = sampleUniformSphere(s.next2D());
      float pdf = 0;
      bsdf.eval(wo, wi, pdf);
      integral += pdf * 4 * kPi;
      const float uLobe = s.next1D();
      const Vec2f u = s.next2D();
      BsdfSample bs;
      if (bsdf.sample(wo, uLobe, u, bs) && !bs.delta) ++succeeded;
    }
    integral /= kN;
    const double rate = double(succeeded) / kN;
    INFO("rough " << in.roughness << " metal " << in.metallic << " trans " << in.transmission << " sheen "
                  << in.sheen_color.x << " integral " << integral << " success rate " << rate);
    CHECK(std::fabs(integral - rate) < 0.02);
  }
}

TEST_CASE("metal and Lambert are reciprocal", "[bsdf]") {
  BsdfInputs metal;
  metal.metallic = 1;
  metal.roughness = 0.4f;
  metal.base_color = {0.9f, 0.6f, 0.3f};
  BsdfInputs lambert;
  lambert.metallic = 0;
  lambert.specular = 0;
  for (const BsdfInputs& in : {metal, lambert}) {
    for (float a : {10.0f, 40.0f, 70.0f}) {
      for (float b : {20.0f, 55.0f}) {
        const Vec3f w1 = dirFromAngle(a, 10), w2 = dirFromAngle(b, 200);
        float p = 0;
        const Vec3f f12 = PrincipledBsdf(in, w1).eval(w1, w2, p) / w2.z;
        const Vec3f f21 = PrincipledBsdf(in, w2).eval(w2, w1, p) / w1.z;
        CHECK(std::fabs(f12.x - f21.x) <= 1e-4f * std::fmax(f12.x, 1.0f));
      }
    }
  }
}

TEST_CASE("sheen albedo table is sane", "[bsdf]") {
  for (float r : {0.0f, 0.5f, 1.0f})
    for (float c : {0.05f, 0.5f, 1.0f}) {
      const float e = sheenAlbedo(c, r);
      CHECK(e >= 0);
      CHECK(e <= 1);
    }
  CHECK(sheenAlbedo(0.5f, 1.0f) > sheenAlbedo(0.5f, 0.1f));  // rougher sheen reflects more
}
