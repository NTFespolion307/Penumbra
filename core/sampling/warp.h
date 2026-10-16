#pragma once

// Mappings from [0,1)^2 to directions and points, with their densities.

#include <cmath>

#include "core/math/vec.h"

namespace pen {

PEN_HD inline Vec2f sampleConcentricDisk(Vec2f u) {
  const float ox = 2 * u.x - 1, oy = 2 * u.y - 1;
  if (ox == 0 && oy == 0) return {0, 0};
  float r, theta;
  if (std::fabs(ox) > std::fabs(oy)) {
    r = ox;
    theta = (kPi / 4) * (oy / ox);
  } else {
    r = oy;
    theta = (kPi / 2) - (kPi / 4) * (ox / oy);
  }
  return {r * std::cos(theta), r * std::sin(theta)};
}

// Cosine-weighted hemisphere around +z; pdf = cos(theta) / pi.
PEN_HD inline Vec3f sampleCosineHemisphere(Vec2f u) {
  const Vec2f d = sampleConcentricDisk(u);
  const float z = safeSqrt(1 - d.x * d.x - d.y * d.y);
  return {d.x, d.y, z};
}
PEN_HD inline float cosineHemispherePdf(float cosTheta) { return cosTheta > 0 ? cosTheta * kInvPi : 0; }

// Uniform sphere; pdf = 1 / (4 pi).
PEN_HD inline Vec3f sampleUniformSphere(Vec2f u) {
  const float z = 1 - 2 * u.x;
  const float r = safeSqrt(1 - z * z);
  const float phi = kTwoPi * u.y;
  return {r * std::cos(phi), r * std::sin(phi), z};
}

// Uniform cone around +z with half-angle acos(cosMax); pdf = 1 / (2 pi (1 - cosMax)).
PEN_HD inline Vec3f sampleUniformCone(Vec2f u, float cosMax) {
  const float cosTheta = (1 - u.x) + u.x * cosMax;
  const float sinTheta = safeSqrt(1 - cosTheta * cosTheta);
  const float phi = kTwoPi * u.y;
  return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

// Uniform point on a triangle as barycentrics (b1, b2) of vertices 1 and 2 (Heitz 2019, low distortion).
PEN_HD inline Vec2f sampleUniformTriangle(Vec2f u) {
  float b0, b1;
  if (u.x < u.y) {
    b0 = u.x / 2;
    b1 = u.y - b0;
  } else {
    b1 = u.y / 2;
    b0 = u.x - b1;
  }
  return {b1, 1 - b0 - b1};
}

PEN_HD inline float powerHeuristic(float pdfA, float pdfB) {
  const float a = pdfA * pdfA, b = pdfB * pdfB;
  return a + b > 0 ? a / (a + b) : 0;
}

}  // namespace pen
