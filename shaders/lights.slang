// Port of core/render/lights.cpp and the alias tables of core/sampling/alias_table.cpp.
#pragma once
#include "scene.slang"

[[vk::binding(14, 0)]] StructuredBuffer<EmissiveTriangle> g_emissive;
[[vk::binding(15, 0)]] StructuredBuffer<int> g_emissive_offset;  // per (instance, primitive) slot: first triangle or -1
[[vk::binding(16, 0)]] StructuredBuffer<AliasEntry> g_light_table;
[[vk::binding(17, 0)]] StructuredBuffer<PunctualLight> g_lights;
[[vk::binding(18, 0)]] StructuredBuffer<float3> g_env_map;
[[vk::binding(19, 0)]] StructuredBuffer<AliasEntry> g_env_rows;  // nv entries, then nv rows of nu column entries
[[vk::binding(22, 0)]] StructuredBuffer<float> g_env_weights;    // nv x nu

static const float kOneMinusEpsilon = 0.99999994;

struct LightSample {
  float3 wi;
  float dist;
  float3 L;
  float pdf;
  bool delta;
  float3 p_light;
  float3 n_light;
};

// AliasTable::sample on a table stored at `base` in `table` with `n` entries.
uint aliasSample(StructuredBuffer<AliasEntry> table, uint base, uint n, float u, out float remapped) {
  float x = u * float(n);
  uint i = min(uint(x), n - 1);
  float f = min(x - float(i), kOneMinusEpsilon);
  AliasEntry e = table[base + i];
  if (f < e.prob) {
    remapped = min(f / e.prob, kOneMinusEpsilon);
    return i;
  }
  remapped = min((f - e.prob) / (1.0 - e.prob), kOneMinusEpsilon);
  return e.alias;
}

// ---- Environment ----

float2 directionToUv(float3 d, float rotation) {
  float u = atan2(-d.z, -d.x) * kInv2Pi + 0.5 - rotation * kInv2Pi;
  u -= floor(u);
  float t = acos(clampf(d.y, -1.0, 1.0)) * kInvPi;
  return float2(u, min(t, kOneMinusEpsilon));
}

float3 uvToDirection(float2 uv, float rotation, out float sinTheta) {
  float phi = (uv.x - 0.5) * kTwoPi + rotation;
  float theta = uv.y * kPi;
  sinTheta = sin(theta);
  return float3(-cos(phi) * sinTheta, cos(theta), -sin(phi) * sinTheta);
}

float3 mapLookup(float2 uv) {
  uint w = g_frame.env_width, h = g_frame.env_height;
  uint x = min(uint(uv.x * w), w - 1);
  uint y = min(uint(uv.y * h), h - 1);
  return g_env_map[y * w + x];
}

float envDistPdf(float2 p) {
  if (g_frame.env_dist_sum <= 0) return 0;
  uint nu = g_frame.env_width, nv = g_frame.env_height;
  uint c = min(uint(p.x * nu), nu - 1);
  uint v = min(uint(p.y * nv), nv - 1);
  return g_env_weights[v * nu + c] / g_frame.env_dist_sum * float(nu) * float(nv);
}

float2 envDistSample(float2 u, out float pdf) {
  uint nu = g_frame.env_width, nv = g_frame.env_height;
  float fv, fu;
  uint v = aliasSample(g_env_rows, 0, nv, u.y, fv);
  uint c = aliasSample(g_env_rows, nv + v * nu, nu, u.x, fu);
  pdf = envDistPdf(float2((c + 0.5) / nu, (v + 0.5) / nv));
  return float2((c + fu) / float(nu), (v + fv) / float(nv));
}

float environmentDirectionPdf(float3 d) {
  if (g_frame.env_has_map == 0) return kInv4Pi;
  float2 uv = directionToUv(d, g_frame.env_rotation);
  float sinTheta = sin(uv.y * kPi);
  if (sinTheta <= 0) return 0;
  return envDistPdf(uv) / (2 * kPi * kPi * sinTheta);
}

float3 environmentRadiance(float3 d) {
  if (g_frame.env_has_map == 0) return g_frame.env_color;
  return mapLookup(directionToUv(d, g_frame.env_rotation)) * g_frame.env_strength;
}

float lightPmf(uint i) { return g_light_table[i].pmf; }

float environmentPdf(float3 d) {
  if (g_frame.env_is_black != 0 || g_frame.light_count == 0) return 0;
  return lightPmf(g_frame.env_light_index) * environmentDirectionPdf(d);
}

// ---- Emissive triangles ----

float emissiveHitPdf(Hit hit, float3 from, SurfaceInteraction si) {
  Instance inst = g_instances[hit.instance];
  int base = g_emissive_offset[inst.slot_base + hit.primitive];
  if (base < 0) return 0;
  EmissiveTriangle tri = g_emissive[uint(base) + hit.triangle];
  if (tri.area <= 0) return 0;
  float3 to = si.p - from;
  float d2 = lengthSquared(to);
  float cosL = abs(dot(tri.normal, to)) / sqrt(d2);
  if (cosL <= 0) return 0;
  return lightPmf(uint(base) + hit.triangle) * d2 / (cosL * tri.area);
}

float3 emissionAt(EmissiveTriangle tri, float b1, float b2) {
  Hit h;
  h.valid = true;
  h.t = 0;
  h.instance = tri.instance;
  h.primitive = tri.primitive;
  h.triangle = tri.triangle;
  h.u = b1;
  h.v = b2;
  return makeSurfaceInteraction(h, tri.normal).emission;
}

bool sampleLight(float3 p, float uSelect, float2 u, out LightSample o) {
  o.wi = float3(0, 0, 0);
  o.dist = 0;
  o.L = float3(0, 0, 0);
  o.pdf = 0;
  o.delta = false;
  o.p_light = float3(0, 0, 0);
  o.n_light = float3(0, 0, 0);
  if (g_frame.light_count == 0 || g_frame.light_sum <= 0) return false;
  float unused;
  uint index = aliasSample(g_light_table, 0, g_frame.light_count, uSelect, unused);
  float pmf = lightPmf(index);
  if (pmf <= 0) return false;
  uint nTri = g_frame.num_emissive, nPunctual = g_frame.num_punctual;

  if (index < nTri) {
    EmissiveTriangle tri = g_emissive[index];
    if (tri.area <= 0) return false;
    float2 b = sampleUniformTriangle(u);
    float3 pl = tri.p0 + tri.e1 * b.x + tri.e2 * b.y;
    float3 to = pl - p;
    float d2 = lengthSquared(to);
    if (d2 <= 0) return false;
    float dist = sqrt(d2);
    o.wi = to / dist;
    float cosL = -dot(tri.normal, o.wi);
    if (cosL <= 0 && tri.double_sided == 0) return false;
    if (cosL == 0) return false;
    o.dist = dist;
    o.L = emissionAt(tri, b.x, b.y);
    o.pdf = pmf * d2 / (abs(cosL) * tri.area);
    o.delta = false;
    o.p_light = pl;
    o.n_light = cosL > 0 ? tri.normal : -tri.normal;
    return !isBlack(o.L);
  }

  if (index < nTri + nPunctual) {
    PunctualLight l = g_lights[index - nTri];
    o.delta = true;
    o.pdf = pmf;
    if (l.type == 2) {
      o.wi = -l.direction;
      o.dist = kInfinity;
      o.L = l.intensity;
      return true;
    }
    float3 to = l.position - p;
    float d2 = lengthSquared(to);
    if (d2 <= 0) return false;
    o.dist = sqrt(d2);
    o.wi = to / o.dist;
    o.p_light = l.position;
    float falloff = 1;
    if (l.type == 1) {
      float cd = dot(l.direction, -o.wi);
      float t = clampf((cd - l.cos_outer) / max(l.cos_inner - l.cos_outer, 1e-4), 0.0, 1.0);
      falloff = t * t;
      if (falloff <= 0) return false;
    }
    o.L = l.intensity * (falloff / d2);
    return true;
  }

  // Environment.
  o.delta = false;
  o.dist = kInfinity;
  if (g_frame.env_has_map != 0) {
    float pdfUv = 0;
    float2 uv = envDistSample(u, pdfUv);
    float sinTheta = 0;
    o.wi = uvToDirection(uv, g_frame.env_rotation, sinTheta);
    if (sinTheta <= 0 || pdfUv <= 0) return false;
    o.pdf = pmf * pdfUv / (2 * kPi * kPi * sinTheta);
  } else {
    o.wi = sampleUniformSphere(u);
    o.pdf = pmf * kInv4Pi;
  }
  o.L = environmentRadiance(o.wi);
  return !isBlack(o.L);
}
