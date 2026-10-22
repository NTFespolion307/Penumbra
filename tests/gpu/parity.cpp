// penumbra-gpu-parity [--backend vulkan|cuda]: runs shaders/parity.slang on the first GPU of that backend (Vulkan:
// with ray tracing) and compares every value with the C++ reference (DECISIONS D-077). Exit code 0 when every function agrees: integer paths bit for bit, discrete
// choices (sample succeeded, delta, lobe) in at least 99.5% of cases, floats within a relative 2e-3 in at least
// 99.5% of cases (the rest are ill-conditioned inputs: grazing angles, total internal reflection edges).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(PEN_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.h"
#endif
#if defined(PEN_HAS_CUDA)
#include "backends/cuda/cuda_backend.h"
#endif
#include "core/sampling/sampler.h"
#include "core/sampling/warp.h"
#include "core/shading/bsdf.h"
#include "core/shading/microfacet.h"

using namespace pen;

namespace {

constexpr uint32_t kValues = 32;
constexpr uint32_t kCases = 65536;

float rnd(uint32_t i, uint32_t k) { return toUnitFloat(hashCombine(hashU32(i * 0x9e3779b9u + 1u), k)); }

Vec3f randomDir(uint32_t i, uint32_t k, bool upper) {
  Vec3f v{rnd(i, k) * 2 - 1, rnd(i, k + 1) * 2 - 1, rnd(i, k + 2) * 2 - 1};
  if (upper) v.z = std::fabs(v.z) + 0.05f;
  return normalize(v);
}

BsdfInputs randomInputs(uint32_t i) {
  BsdfInputs b;
  b.base_color = {rnd(i, 20), rnd(i, 21), rnd(i, 22)};
  b.metallic = rnd(i, 23) < 0.3f ? 1.0f : (rnd(i, 23) < 0.6f ? 0.0f : rnd(i, 24));
  b.roughness = rnd(i, 25) < 0.1f ? 0.0f : rnd(i, 26);
  b.transmission = rnd(i, 27) < 0.5f ? 0.0f : rnd(i, 28);
  b.ior = 1.0f + rnd(i, 29) * 1.5f;
  b.specular = rnd(i, 30);
  b.specular_color = {rnd(i, 31), rnd(i, 32), rnd(i, 33)};
  b.clearcoat = rnd(i, 34) < 0.6f ? 0.0f : rnd(i, 35);
  b.clearcoat_roughness = rnd(i, 36);
  b.sheen_color = rnd(i, 37) < 0.6f ? Vec3f(0) : Vec3f(rnd(i, 38), rnd(i, 39), rnd(i, 40));
  b.sheen_roughness = rnd(i, 41);
  b.anisotropy = rnd(i, 42) < 0.7f ? 0.0f : rnd(i, 43);
  return b;
}

void reference(uint32_t i, float* o) {
  for (uint32_t k = 0; k < kValues; ++k) o[k] = 0;
  const Vec2f s = sobolOwen2D(i, hashU32(i));
  o[0] = s.x;
  o[1] = s.y;
  const Vec3f c = sampleCosineHemisphere({rnd(i, 0), rnd(i, 1)});
  o[2] = c.x;
  o[3] = c.y;
  o[4] = c.z;
  o[5] = fresnelDielectric(rnd(i, 2) * 2 - 1, 1 + rnd(i, 3) * 2);
  GGX g;
  g.ax = 0.01f + rnd(i, 4);
  g.ay = 0.01f + rnd(i, 5);
  const Vec3f wo = randomDir(i, 6, true), wi = randomDir(i, 9, true), h = normalize(wo + wi);
  o[6] = g.D(h);
  o[7] = g.G(wo, wi);
  o[8] = g.Dvisible(wo, h);
  const Vec3f m = g.sampleVisible(wo, {rnd(i, 12), rnd(i, 13)});
  o[9] = m.x;
  o[10] = m.y;
  o[11] = m.z;
  const Vec3f bwo = randomDir(i, 14, rnd(i, 17) < 0.8f), bwi = randomDir(i, 44, false);
  const PrincipledBsdf b(randomInputs(i), bwo);
  float pdf = 0;
  const Vec3f f = b.eval(bwo, bwi, pdf);
  o[12] = f.x;
  o[13] = f.y;
  o[14] = f.z;
  o[15] = pdf;
  BsdfSample bs;
  const bool ok = b.sample(bwo, rnd(i, 18), {rnd(i, 19), rnd(i, 47)}, bs);
  o[16] = ok ? 1.0f : 0.0f;
  if (ok) {
    o[17] = bs.wi.x;
    o[18] = bs.wi.y;
    o[19] = bs.wi.z;
    o[20] = bs.weight.x;
    o[21] = bs.weight.y;
    o[22] = bs.weight.z;
    o[23] = bs.pdf;
    o[24] = bs.delta ? 1.0f : 0.0f;
    o[25] = static_cast<float>(bs.kind);
  }
  o[26] = b.isDelta() ? 1.0f : 0.0f;
  const Frame fr(randomDir(i, 48, false));
  const Vec3f l = fr.toLocal(randomDir(i, 51, false));
  o[27] = l.x;
  o[28] = l.y;
  o[29] = l.z;
  o[30] = sheenAlbedo(rnd(i, 54), rnd(i, 55));
  const uint32_t bits = hashCombine(i, 7);
  std::memcpy(&o[31], &bits, 4);
}

struct Group {
  const char* name;
  uint32_t first, last;  // value indices, inclusive
  enum Kind { Exact, Discrete, Float } kind;
};

const Group kGroups[] = {
    {"sobolOwen2D", 0, 1, Group::Exact},
    {"sampleCosineHemisphere", 2, 4, Group::Float},
    {"fresnelDielectric", 5, 5, Group::Float},
    {"GGX D/G/Dvisible", 6, 8, Group::Float},
    {"GGX sampleVisible", 9, 11, Group::Float},
    {"BSDF eval (f, pdf)", 12, 15, Group::Float},
    {"BSDF sample succeeded", 16, 16, Group::Discrete},
    {"BSDF sample (wi, weight, pdf)", 17, 23, Group::Float},
    {"BSDF sample delta / lobe", 24, 25, Group::Discrete},
    {"BSDF isDelta", 26, 26, Group::Discrete},
    {"Frame", 27, 29, Group::Float},
    {"sheenAlbedo", 30, 30, Group::Float},
    {"integer hash", 31, 31, Group::Exact},
};

}  // namespace

int main(int argc, char** argv) {
  const std::string backend = argc >= 3 && std::string(argv[1]) == "--backend" ? argv[2] : "vulkan";
  std::vector<float> got;
  try {
    if (backend == "vulkan") {
#if defined(PEN_HAS_VULKAN)
      const VulkanProbe& probe = probeVulkan();
      int gpu = -1;
      for (const VulkanGpu& g : probe.gpus)
        if (g.ray_tracing) {
          gpu = static_cast<int>(g.physical_index);
          std::printf("GPU: %s (%s), backend vulkan\n", g.name.c_str(), g.driver.c_str());
          break;
        }
      if (gpu < 0) {
        std::printf("no GPU with Vulkan ray tracing: %s\n", probe.error.empty() ? "none found" : probe.error.c_str());
        return 1;
      }
      got = runVulkanParity(static_cast<uint32_t>(gpu), kCases);
#else
      std::printf("this build has no Vulkan backend\n");
      return 1;
#endif
    } else if (backend == "cuda") {
#if defined(PEN_HAS_CUDA)
      const CudaProbe& probe = probeCuda();
      int gpu = -1;
      for (const CudaGpu& g : probe.gpus)
        if (g.usable) {
          gpu = static_cast<int>(g.ordinal);
          std::printf("GPU: %s (compute %d.%d), backend cuda, %s\n", g.name.c_str(), g.cc_major, g.cc_minor,
                      cudaBuildSummary().c_str());
          break;
        }
      if (gpu < 0) {
        std::printf("no usable CUDA GPU: %s\n", probe.error.empty() ? "none found" : probe.error.c_str());
        return 1;
      }
      got = runCudaParity(static_cast<uint32_t>(gpu), kCases);
#else
      std::printf("this build has no CUDA backend\n");
      return 1;
#endif
    } else {
      std::printf("usage: penumbra-gpu-parity [--backend vulkan|cuda]\n");
      return 2;
    }
  } catch (const std::exception& e) {
    std::printf("GPU run failed: %s\n", e.what());
    return 1;
  }

  bool ok = true;
  std::vector<float> want(kValues);
  for (const Group& grp : kGroups) {
    uint32_t checked = 0, bad = 0;
    double worst = 0;
    for (uint32_t i = 0; i < kCases; ++i) {
      reference(i, want.data());
      const float* g = &got[size_t(i) * kValues];
      // Sample outputs only count when both sides sampled successfully.
      if (grp.first >= 17 && grp.last <= 25 && (want[16] != 1 || g[16] != 1)) continue;
      bool caseBad = false;
      for (uint32_t k = grp.first; k <= grp.last; ++k) {
        const float a = want[k], b = g[k];
        if (grp.kind == Group::Exact) {
          caseBad = caseBad || std::memcmp(&a, &b, 4) != 0;
        } else if (grp.kind == Group::Discrete) {
          caseBad = caseBad || a != b;
        } else {
          if (std::isnan(a) != std::isnan(b) || std::isinf(a) != std::isinf(b)) {
            caseBad = true;
            continue;
          }
          if (!std::isfinite(a)) continue;
          const double rel = std::fabs(double(a) - b) / std::fmax(std::fmax(std::fabs(a), std::fabs(b)), 1e-2);
          worst = std::fmax(worst, rel);
          caseBad = caseBad || rel > 2e-3;
        }
      }
      ++checked;
      bad += caseBad;
    }
    const double frac = checked ? double(bad) / checked : 0;
    const bool pass = grp.kind == Group::Exact ? bad == 0 : frac <= 0.005;
    ok = ok && pass;
    std::printf("  %-4s %-30s %6u cases, %5u differ (%.3f%%), worst relative error %.2e\n", pass ? "ok" : "FAIL",
                grp.name, checked, bad, 100 * frac, worst);
  }
  std::printf(ok ? "parity OK\n" : "parity FAILED\n");
  return ok ? 0 : 1;
}
