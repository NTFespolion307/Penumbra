#pragma once

// The atmosphere (M14, DECISIONS D-119): exponential height layers, density exp(-(y - base) / height). Optical
// depth along a ray has a closed form, so transmittance is exact and free-flight distances are sampled by
// inverting it; no ray marching. RGB coefficients: a colour channel is picked at random and the distance sampled
// for it, and the weights divide by the average over the three channels (spectral MIS), so coloured air stays
// unbiased. Written once for the CPU; shaders/medium.slang is its twin for Vulkan and CUDA.

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/math/vec.h"
#include "core/scene/scene.h"

namespace pen {

namespace medium_detail {

constexpr float kInf = std::numeric_limits<float>::infinity();

// ln of the layer's density at y (clamped so a point far below the base does not overflow).
inline float logDensity(const MediumLayer& l, float y) { return std::fmin(-(y - l.base) * l.inv_height, 80.0f); }

}  // namespace medium_detail

// Density-weighted length of the ray o + s d, s in [0, t] (d unit): optical depth per unit of sigma_t. Infinite
// for an unbounded ray that is not going up.
inline float layerDepth(const MediumLayer& l, Vec3f o, Vec3f d, float t) {
  const float k = d.y * l.inv_height;  // the density changes as exp(-k s) along the ray
  const float a = std::exp(medium_detail::logDensity(l, o.y));
  if (std::isinf(t)) return k > 0 ? a / k : medium_detail::kInf;
  if (std::fabs(k * t) < 1e-6f) return a * t;
  return a * (-std::expm1(-k * t)) / k;
}

// The distance at which layerDepth reaches `depth`, or infinity if the ray never gets that far.
inline float layerDistance(const MediumLayer& l, Vec3f o, Vec3f d, float depth) {
  const float k = d.y * l.inv_height;
  const float x = depth / std::exp(medium_detail::logDensity(l, o.y));
  if (std::fabs(k * x) < 1e-6f) return x;
  if (k * x >= 1) return medium_detail::kInf;  // going up: the layer thins out before the depth is reached
  return -std::log1p(-k * x) / k;
}

// Transmittance along o + s d, s in [0, t] (t may be infinite).
inline Vec3f mediumTransmittance(const Medium& m, Vec3f o, Vec3f d, float t) {
  Vec3f tau(0);
  for (uint32_t i = 0; i < m.count; ++i) {
    const float depth = layerDepth(m.layers[i], o, d, t);
    if (std::isinf(depth)) {
      const Vec3f& s = m.layers[i].sigma_t;
      tau = tau + Vec3f(s.x > 0 ? depth : 0.0f, s.y > 0 ? depth : 0.0f, s.z > 0 ? depth : 0.0f);
    } else {
      tau = tau + m.layers[i].sigma_t * depth;
    }
  }
  return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
}

struct MediumEvent {
  bool scattered = false;  // a collision before tmax: scatter at p with `layer`'s phase function
  float t = 0;
  uint32_t layer = 0;
  Vec3f weight{1, 1, 1};   // throughput factor for this segment (transmittance and scattering over the pdf)
};

// Free-flight sampling on [0, tmax): uChannel picks the colour channel, u[i] the collision distance of layer i
// (the first collision of several independent layers is the earliest of theirs).
inline MediumEvent sampleMedium(const Medium& m, Vec3f o, Vec3f d, float tmax, float uChannel, const float* u) {
  MediumEvent ev;
  const uint32_t c = std::min(static_cast<uint32_t>(uChannel * 3), 2u);
  float tMin = medium_detail::kInf;
  for (uint32_t i = 0; i < m.count; ++i) {
    const float sigma = m.layers[i].sigma_t[static_cast<int>(c)];
    if (sigma <= 0) continue;
    const float t = layerDistance(m.layers[i], o, d, -std::log1p(-std::fmin(u[i], 0.99999994f)) / sigma);
    if (t < tMin) {
      tMin = t;
      ev.layer = i;
    }
  }
  if (tMin < tmax) {
    const MediumLayer& l = m.layers[ev.layer];
    const Vec3f T = mediumTransmittance(m, o, d, tMin);
    const float density = std::exp(medium_detail::logDensity(l, o.y + tMin * d.y));
    const Vec3f collide = T * l.sigma_t * density;  // pdf of this collision, per channel
    const float pdf = (collide.x + collide.y + collide.z) / 3;
    ev.scattered = true;
    ev.t = tMin;
    ev.weight = pdf > 0 ? T * l.sigma_s * (density / pdf) : Vec3f(0);
    return ev;
  }
  const Vec3f T = mediumTransmittance(m, o, d, tmax);
  const float pdf = (T.x + T.y + T.z) / 3;  // probability of no collision, per channel, averaged
  ev.weight = pdf > 0 ? T / pdf : Vec3f(0);
  return ev;
}

// Phase function value for scattering from propagation direction `in` into `out` (both unit).
inline float phaseValue(const MediumLayer& l, Vec3f in, Vec3f out) {
  const float c = dot(in, out);
  if (l.rayleigh) return 3.0f / (16.0f * kPi) * (1 + c * c);
  const float g = l.g;
  const float denom = 1 + g * g - 2 * g * c;
  return kInv4Pi * (1 - g * g) / (denom * std::sqrt(denom));
}

// Samples the phase function around `in`; the pdf equals phaseValue (both are sampled exactly).
inline Vec3f samplePhase(const MediumLayer& l, Vec3f in, Vec2f u, float& pdf) {
  float c;
  if (l.rayleigh) {
    // Inverts the CDF (c^3 + 3c + 4) / 8 = u with Cardano's formula.
    const float h = 4 * u.x - 2;
    const float a = std::cbrt(h + std::sqrt(h * h + 1));
    c = a - 1 / a;
  } else if (std::fabs(l.g) < 1e-3f) {
    c = 1 - 2 * u.x;
  } else {
    const float g = l.g;
    const float s = (1 - g * g) / (1 - g + 2 * g * u.x);
    c = (1 + g * g - s * s) / (2 * g);
  }
  c = std::fmax(-1.0f, std::fmin(1.0f, c));
  const float sinT = std::sqrt(std::fmax(0.0f, 1 - c * c));
  const float phi = 2 * kPi * u.y;
  const Vec3f out = Frame(in).toWorld({sinT * std::cos(phi), sinT * std::sin(phi), c});
  pdf = phaseValue(l, in, out);
  return out;
}

}  // namespace pen
