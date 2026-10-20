// GPU parity test (DECISIONS D-077): evaluates the shading functions on inputs derived from integer hashes, so
// tests/gpu/parity.cpp can compute the same values with the C++ reference and compare them function by function.
// Output: kParityValues floats per case; the layout must match tests/gpu/parity.cpp.
#include "bsdf.slang"
#include "sampler.slang"

[[vk::binding(0, 0)]] RWStructuredBuffer<float> g_parity_out;

struct ParityPush {
  uint count;
};
[[vk::push_constant]] ConstantBuffer<ParityPush> g_parity;

static const uint kParityValues = 32;

float rnd(uint i, uint k) { return toUnitFloat(hashCombine(hashU32(i * 0x9e3779b9u + 1u), k)); }

float3 randomDir(uint i, uint k, bool upper) {
  float3 v = float3(rnd(i, k) * 2 - 1, rnd(i, k + 1) * 2 - 1, rnd(i, k + 2) * 2 - 1);
  if (upper) v.z = abs(v.z) + 0.05;
  return normalizeSafe(v);
}

BsdfInputs randomInputs(uint i) {
  BsdfInputs b;
  b.base_color = float3(rnd(i, 20), rnd(i, 21), rnd(i, 22));
  b.metallic = rnd(i, 23) < 0.3 ? 1.0 : (rnd(i, 23) < 0.6 ? 0.0 : rnd(i, 24));
  b.roughness = rnd(i, 25) < 0.1 ? 0.0 : rnd(i, 26);
  b.transmission = rnd(i, 27) < 0.5 ? 0.0 : rnd(i, 28);
  b.ior = 1.0 + rnd(i, 29) * 1.5;
  b.specular = rnd(i, 30);
  b.specular_color = float3(rnd(i, 31), rnd(i, 32), rnd(i, 33));
  b.clearcoat = rnd(i, 34) < 0.6 ? 0.0 : rnd(i, 35);
  b.clearcoat_roughness = rnd(i, 36);
  b.sheen_color = rnd(i, 37) < 0.6 ? float3(0, 0, 0) : float3(rnd(i, 38), rnd(i, 39), rnd(i, 40));
  b.sheen_roughness = rnd(i, 41);
  b.anisotropy = rnd(i, 42) < 0.7 ? 0.0 : rnd(i, 43);
  return b;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void parity(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= g_parity.count) return;
  float o[kParityValues];
  for (uint k = 0; k < kParityValues; ++k) o[k] = 0;

  float2 s = sobolOwen2D(i, hashU32(i));
  o[0] = s.x;
  o[1] = s.y;
  float3 c = sampleCosineHemisphere(float2(rnd(i, 0), rnd(i, 1)));
  o[2] = c.x;
  o[3] = c.y;
  o[4] = c.z;
  o[5] = fresnelDielectric(rnd(i, 2) * 2 - 1, 1 + rnd(i, 3) * 2);

  GGX g = makeGGX(0.01 + rnd(i, 4), 0.01 + rnd(i, 5));
  float3 wo = randomDir(i, 6, true), wi = randomDir(i, 9, true), h = normalizeSafe(wo + wi);
  o[6] = g.D(h);
  o[7] = g.G(wo, wi);
  o[8] = g.Dvisible(wo, h);
  float3 m = g.sampleVisible(wo, float2(rnd(i, 12), rnd(i, 13)));
  o[9] = m.x;
  o[10] = m.y;
  o[11] = m.z;

  // The BSDF sees directions on both sides (transmission, inside).
  float3 bwo = randomDir(i, 14, rnd(i, 17) < 0.8), bwi = randomDir(i, 44, false);
  PrincipledBsdf b = makeBsdf(randomInputs(i), bwo);
  float pdf;
  float3 f = b.eval(bwo, bwi, pdf);
  o[12] = f.x;
  o[13] = f.y;
  o[14] = f.z;
  o[15] = pdf;
  BsdfSample bs;
  bool ok = b.sample(bwo, rnd(i, 18), float2(rnd(i, 19), rnd(i, 47)), bs);
  o[16] = ok ? 1 : 0;
  if (ok) {
    o[17] = bs.wi.x;
    o[18] = bs.wi.y;
    o[19] = bs.wi.z;
    o[20] = bs.weight.x;
    o[21] = bs.weight.y;
    o[22] = bs.weight.z;
    o[23] = bs.pdf;
    o[24] = bs.delta ? 1 : 0;
    o[25] = float(bs.kind);
  }
  o[26] = b.isDelta() ? 1 : 0;
  Frame fr = frameFromNormal(randomDir(i, 48, false));
  float3 l = fr.toLocal(randomDir(i, 51, false));
  o[27] = l.x;
  o[28] = l.y;
  o[29] = l.z;
  o[30] = sheenAlbedo(rnd(i, 54), rnd(i, 55));
  o[31] = asfloat(hashCombine(i, 7));  // integer path, must be exact

  for (uint k = 0; k < kParityValues; ++k) g_parity_out[i * kParityValues + k] = o[k];
}
