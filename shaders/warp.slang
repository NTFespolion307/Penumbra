// Port of core/sampling/warp.h.
#pragma once
#include "math.slang"

float2 sampleConcentricDisk(float2 u) {
  float ox = 2 * u.x - 1, oy = 2 * u.y - 1;
  if (ox == 0 && oy == 0) return float2(0, 0);
  float r, theta;
  if (abs(ox) > abs(oy)) {
    r = ox;
    theta = (kPi / 4) * (oy / ox);
  } else {
    r = oy;
    theta = (kPi / 2) - (kPi / 4) * (ox / oy);
  }
  return float2(r * cos(theta), r * sin(theta));
}

float3 sampleCosineHemisphere(float2 u) {
  float2 d = sampleConcentricDisk(u);
  float z = safeSqrt(1 - d.x * d.x - d.y * d.y);
  return float3(d.x, d.y, z);
}

float3 sampleUniformSphere(float2 u) {
  float z = 1 - 2 * u.x;
  float r = safeSqrt(1 - z * z);
  float phi = kTwoPi * u.y;
  return float3(r * cos(phi), r * sin(phi), z);
}

float2 sampleUniformTriangle(float2 u) {
  float b0, b1;
  if (u.x < u.y) {
    b0 = u.x / 2;
    b1 = u.y - b0;
  } else {
    b1 = u.y / 2;
    b0 = u.x - b1;
  }
  return float2(b1, 1 - b0 - b1);
}

float powerHeuristic(float pdfA, float pdfB) {
  float a = pdfA * pdfA, b = pdfB * pdfB;
  return a + b > 0 ? a / (a + b) : 0;
}
