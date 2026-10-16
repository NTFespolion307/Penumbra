#pragma once

// GGX (Trowbridge-Reitz) microfacet distribution with anisotropy, Smith height-correlated masking-shadowing,
// visible-normal sampling (Heitz 2018), and dielectric Fresnel. All directions in the local shading frame
// (normal = +z). Conventions follow pbrt-v4.

#include <cmath>

#include "core/math/vec.h"

namespace pen {

PEN_HD inline float cosTheta(Vec3f w) { return w.z; }
PEN_HD inline float absCosTheta(Vec3f w) { return std::fabs(w.z); }
PEN_HD inline bool sameHemisphere(Vec3f a, Vec3f b) { return a.z * b.z > 0; }
PEN_HD inline Vec3f reflect(Vec3f wo, Vec3f n) { return -wo + n * (2 * dot(wo, n)); }

// Refracts wi through a surface with normal n (on either side). eta = n_t / n_i for n on the incident side.
// Returns false on total internal reflection. etap receives the relative IOR actually used.
PEN_HD inline bool refract(Vec3f wi, Vec3f n, float eta, float& etap, Vec3f& wt) {
  float cosI = dot(n, wi);
  if (cosI < 0) {
    eta = 1 / eta;
    cosI = -cosI;
    n = -n;
  }
  const float sin2I = std::fmax(0.0f, 1 - cosI * cosI);
  const float sin2T = sin2I / (eta * eta);
  if (sin2T >= 1) return false;
  const float cosT = safeSqrt(1 - sin2T);
  wt = -wi / eta + n * (cosI / eta - cosT);
  etap = eta;
  return true;
}

// Unpolarized Fresnel reflectance of a dielectric interface; cosI may be negative (inside), eta = n_t / n_i.
PEN_HD inline float fresnelDielectric(float cosI, float eta) {
  cosI = clamp(cosI, -1.0f, 1.0f);
  if (cosI < 0) {
    eta = 1 / eta;
    cosI = -cosI;
  }
  const float sin2T = (1 - cosI * cosI) / (eta * eta);
  if (sin2T >= 1) return 1.0f;
  const float cosT = safeSqrt(1 - sin2T);
  const float rParl = (eta * cosI - cosT) / (eta * cosI + cosT);
  const float rPerp = (cosI - eta * cosT) / (cosI + eta * cosT);
  return 0.5f * (rParl * rParl + rPerp * rPerp);
}

PEN_HD inline float schlickWeight(float cosTheta) {
  const float m = clamp(1 - cosTheta, 0.0f, 1.0f);
  const float m2 = m * m;
  return m2 * m2 * m;
}
PEN_HD inline Vec3f fresnelSchlick(Vec3f f0, float cosTheta) { return f0 + (Vec3f(1) - f0) * schlickWeight(cosTheta); }
PEN_HD inline float fresnelSchlick(float f0, float cosTheta) { return f0 + (1 - f0) * schlickWeight(cosTheta); }

struct GGX {
  float ax = 1e-3f, ay = 1e-3f;

  // Below this roughness the lobe is treated as a perfect mirror / refractor.
  static constexpr float kDeltaAlpha = 1e-3f;
  PEN_HD bool isDelta() const { return ax < kDeltaAlpha && ay < kDeltaAlpha; }

  PEN_HD float D(Vec3f h) const {
    if (h.z <= 0) return 0;
    const float e = sqr(h.x / ax) + sqr(h.y / ay) + sqr(h.z);
    return 1.0f / (kPi * ax * ay * e * e);
  }
  PEN_HD float lambda(Vec3f w) const {
    if (w.z == 0) return 1e30f;
    const float a2 = (sqr(w.x * ax) + sqr(w.y * ay)) / sqr(w.z);
    return 0.5f * (std::sqrt(1 + a2) - 1);
  }
  PEN_HD float G1(Vec3f w) const { return 1 / (1 + lambda(w)); }
  PEN_HD float G(Vec3f wo, Vec3f wi) const { return 1 / (1 + lambda(wo) + lambda(wi)); }
  // Distribution of normals visible from w.
  PEN_HD float Dvisible(Vec3f w, Vec3f h) const {
    const float c = absCosTheta(w);
    return c > 0 ? G1(w) / c * D(h) * std::fabs(dot(w, h)) : 0;
  }
  // Samples a visible microfacet normal for direction w (either hemisphere); the result has h.z >= 0.
  PEN_HD Vec3f sampleVisible(Vec3f w, Vec2f u) const {
    Vec3f wh = normalize(Vec3f(ax * w.x, ay * w.y, w.z));
    if (wh.z < 0) wh = -wh;
    const float lensq = wh.x * wh.x + wh.y * wh.y;
    const Vec3f t1 = lensq > 0 ? Vec3f(-wh.y, wh.x, 0) / std::sqrt(lensq) : Vec3f(1, 0, 0);
    const Vec3f t2 = cross(wh, t1);
    const float r = std::sqrt(u.x), phi = kTwoPi * u.y;
    const float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);
    const float s = 0.5f * (1 + wh.z);
    p2 = (1 - s) * safeSqrt(1 - p1 * p1) + s * p2;
    const Vec3f nh = t1 * p1 + t2 * p2 + wh * safeSqrt(1 - p1 * p1 - p2 * p2);
    return normalize(Vec3f(ax * nh.x, ay * nh.y, std::fmax(1e-6f, nh.z)));
  }
};

}  // namespace pen
