#include "core/render/lights.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/sampling/warp.h"

namespace pen {

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

// Equirectangular mapping matching Blender's environment texture, with glTF's +Y up (Blender +Z) and
// Blender -Y forward mapped to glTF +Z. u in [0,1) is azimuth, t in [0,1) is the polar angle from +Y.
Vec2f directionToUv(Vec3f d, float rotation) {
  float u = std::atan2(-d.z, -d.x) * kInv2Pi + 0.5f - rotation * kInv2Pi;
  u -= std::floor(u);
  const float t = std::acos(clamp(d.y, -1.0f, 1.0f)) * kInvPi;
  return {u, std::fmin(t, 0.99999994f)};
}

Vec3f uvToDirection(Vec2f uv, float rotation, float& sinTheta) {
  const float phi = (uv.x - 0.5f) * kTwoPi + rotation;
  const float theta = uv.y * kPi;
  sinTheta = std::sin(theta);
  return {-std::cos(phi) * sinTheta, std::cos(theta), -std::sin(phi) * sinTheta};
}

Vec3f mapLookup(const ImageRGB& map, Vec2f uv) {
  const uint32_t x = std::min(static_cast<uint32_t>(uv.x * map.width), map.width - 1);
  const uint32_t y = std::min(static_cast<uint32_t>(uv.y * map.height), map.height - 1);
  return map.at(x, y);
}

float environmentDirectionPdf(const Environment& env, Vec3f d) {
  if (!env.hasMap()) return kInv4Pi;
  const Vec2f uv = directionToUv(d, env.rotation);
  const float sinTheta = std::sin(uv.y * kPi);
  if (sinTheta <= 0) return 0;
  return env.sampling.pdf(uv) / (2 * kPi * kPi * sinTheta);
}

// Emission at a point on an emissive triangle (texture-aware).
Vec3f emissionAt(const Scene& scene, const EmissiveTriangle& tri, float b1, float b2) {
  Hit h;
  h.valid = true;
  h.instance = tri.instance;
  h.primitive = tri.primitive;
  h.triangle = tri.triangle;
  h.u = b1;
  h.v = b2;
  return makeSurfaceInteraction(scene, h, tri.normal).emission;
}

}  // namespace

Vec3f environmentRadiance(const Environment& env, Vec3f d) {
  if (!env.hasMap()) return env.color;
  return mapLookup(env.map, directionToUv(d, env.rotation)) * env.strength;
}

float environmentPdf(const Scene& scene, Vec3f d) {
  if (scene.environment.isBlack() || scene.light_table.empty()) return 0;
  return scene.light_table.pmf(scene.environmentLightIndex()) * environmentDirectionPdf(scene.environment, d);
}

float emissiveHitPdf(const Scene& scene, const Hit& hit, Vec3f from, const SurfaceInteraction& si) {
  const int32_t base = scene.emissive_offset[hit.instance][hit.primitive];
  if (base < 0) return 0;
  const EmissiveTriangle& tri = scene.emissive_triangles[base + hit.triangle];
  if (tri.area <= 0) return 0;
  const Vec3f to = si.p - from;
  const float d2 = lengthSquared(to);
  const float cosL = std::fabs(dot(tri.normal, to)) / std::sqrt(d2);
  if (cosL <= 0) return 0;
  return scene.light_table.pmf(static_cast<uint32_t>(base) + hit.triangle) * d2 / (cosL * tri.area);
}

bool sampleLight(const Scene& scene, Vec3f p, float uSelect, Vec2f u, LightSample& out) {
  if (scene.light_table.empty() || scene.light_table.sum() <= 0) return false;
  const uint32_t index = scene.light_table.sample(uSelect);
  const float pmf = scene.light_table.pmf(index);
  if (pmf <= 0) return false;
  const uint32_t nTri = static_cast<uint32_t>(scene.emissive_triangles.size());
  const uint32_t nPunctual = static_cast<uint32_t>(scene.lights.size());

  if (index < nTri) {
    const EmissiveTriangle& tri = scene.emissive_triangles[index];
    if (tri.area <= 0) return false;
    const Vec2f b = sampleUniformTriangle(u);
    const Vec3f pl = tri.p0 + tri.e1 * b.x + tri.e2 * b.y;
    const Vec3f to = pl - p;
    const float d2 = lengthSquared(to);
    if (d2 <= 0) return false;
    const float dist = std::sqrt(d2);
    out.wi = to / dist;
    const float cosL = -dot(tri.normal, out.wi);  // > 0 when p sees the front face
    if (cosL <= 0 && !tri.double_sided) return false;
    if (cosL == 0) return false;
    out.dist = dist;
    out.L = emissionAt(scene, tri, b.x, b.y);
    out.pdf = pmf * d2 / (std::fabs(cosL) * tri.area);
    out.delta = false;
    out.p_light = pl;
    out.n_light = cosL > 0 ? tri.normal : -tri.normal;
    return !isBlack(out.L);
  }

  if (index < nTri + nPunctual) {
    const PunctualLight& l = scene.lights[index - nTri];
    out.delta = true;
    out.pdf = pmf;
    if (l.type == LightType::Directional) {
      out.wi = -l.direction;
      out.dist = kInfinity;
      out.L = l.intensity;
      return true;
    }
    const Vec3f to = l.position - p;
    const float d2 = lengthSquared(to);
    if (d2 <= 0) return false;
    out.dist = std::sqrt(d2);
    out.wi = to / out.dist;
    out.p_light = l.position;
    float falloff = 1;
    if (l.type == LightType::Spot) {
      const float cd = dot(l.direction, -out.wi);
      const float t = clamp((cd - l.cos_outer) / std::fmax(l.cos_inner - l.cos_outer, 1e-4f), 0.0f, 1.0f);
      falloff = t * t;
      if (falloff <= 0) return false;
    }
    out.L = l.intensity * (falloff / d2);
    return true;
  }

  // Environment.
  const Environment& env = scene.environment;
  out.delta = false;
  out.dist = kInfinity;
  if (env.hasMap()) {
    float pdfUv = 0;
    const Vec2f uv = env.sampling.sample(u, pdfUv);
    float sinTheta = 0;
    out.wi = uvToDirection(uv, env.rotation, sinTheta);
    if (sinTheta <= 0 || pdfUv <= 0) return false;
    out.pdf = pmf * pdfUv / (2 * kPi * kPi * sinTheta);
  } else {
    out.wi = sampleUniformSphere(u);
    out.pdf = pmf * kInv4Pi;
  }
  out.L = environmentRadiance(env, out.wi);
  return !isBlack(out.L);
}

}  // namespace pen
