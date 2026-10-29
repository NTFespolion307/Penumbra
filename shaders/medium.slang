// The atmosphere on the GPU (M14): port of core/render/medium.h, reading the layers from g_frame.medium.
// Exponential height layers, closed-form optical depth, free-flight sampling with spectral MIS (DECISIONS D-119).
#pragma once
#include "scene.slang"

// expm1, log1p and cbrt are not in Slang's core library: a series for small arguments, where the plain forms
// lose the digits.
float expm1f(float x) { return abs(x) < 1e-4 ? x + 0.5 * x * x : exp(x) - 1; }
float log1pf(float x) { return abs(x) < 1e-4 ? x - 0.5 * x * x : log(1 + x); }

float mediumLogDensity(MediumLayer l, float y) { return min(-(y - l.base) * l.inv_height, 80.0); }

float layerDepth(MediumLayer l, float3 o, float3 d, float t) {
  float k = d.y * l.inv_height;
  float a = exp(mediumLogDensity(l, o.y));
  if (isinf(t)) return k > 0 ? a / k : kInfinity;
  if (abs(k * t) < 1e-6) return a * t;
  return a * (-expm1f(-k * t)) / k;
}

float layerDistance(MediumLayer l, float3 o, float3 d, float depth) {
  float k = d.y * l.inv_height;
  float x = depth / exp(mediumLogDensity(l, o.y));
  if (abs(k * x) < 1e-6) return x;
  if (k * x >= 1) return kInfinity;
  return -log1pf(-k * x) / k;
}

float3 mediumTransmittance(float3 o, float3 d, float t) {
  float3 tau = float3(0, 0, 0);
  for (uint i = 0; i < g_frame.medium_count; ++i) {
    MediumLayer l = g_frame.medium[i];
    float depth = layerDepth(l, o, d, t);
    if (isinf(depth)) {
      tau += float3(l.sigma_t.x > 0 ? depth : 0.0, l.sigma_t.y > 0 ? depth : 0.0, l.sigma_t.z > 0 ? depth : 0.0);
    } else {
      tau += l.sigma_t * depth;
    }
  }
  return float3(exp(-tau.x), exp(-tau.y), exp(-tau.z));
}

struct MediumEvent {
  bool scattered;
  float t;
  uint layer;
  float3 weight;
};

MediumEvent sampleMedium(float3 o, float3 d, float tmax, float uChannel, float u[3]) {
  MediumEvent ev;
  ev.scattered = false;
  ev.t = 0;
  ev.layer = 0;
  ev.weight = float3(1, 1, 1);
  uint c = min(uint(uChannel * 3), 2u);
  float tMin = kInfinity;
  for (uint i = 0; i < g_frame.medium_count; ++i) {
    MediumLayer l = g_frame.medium[i];
    float sigma = l.sigma_t[c];
    if (sigma <= 0) continue;
    float t = layerDistance(l, o, d, -log1pf(-min(u[i], 0.99999994)) / sigma);
    if (t < tMin) {
      tMin = t;
      ev.layer = i;
    }
  }
  if (tMin < tmax) {
    MediumLayer l = g_frame.medium[ev.layer];
    float3 T = mediumTransmittance(o, d, tMin);
    float density = exp(mediumLogDensity(l, o.y + tMin * d.y));
    float3 collide = T * l.sigma_t * density;
    float pdf = (collide.x + collide.y + collide.z) / 3;
    ev.scattered = true;
    ev.t = tMin;
    ev.weight = pdf > 0 ? T * l.sigma_s * (density / pdf) : float3(0, 0, 0);
    return ev;
  }
  float3 T = mediumTransmittance(o, d, tmax);
  float pdf = (T.x + T.y + T.z) / 3;
  ev.weight = pdf > 0 ? T / pdf : float3(0, 0, 0);
  return ev;
}

float phaseValue(MediumLayer l, float3 dirIn, float3 dirOut) {
  float c = dot(dirIn, dirOut);
  if (l.rayleigh != 0) return 3.0 / (16.0 * kPi) * (1 + c * c);
  float g = l.g;
  float denom = 1 + g * g - 2 * g * c;
  return kInv4Pi * (1 - g * g) / (denom * sqrt(denom));
}

float3 samplePhase(MediumLayer l, float3 dirIn, float2 u, out float pdf) {
  float c;
  if (l.rayleigh != 0) {
    float h = 4 * u.x - 2;
    float a = pow(h + sqrt(h * h + 1), 1.0 / 3.0);  // the argument is always positive
    c = a - 1 / a;
  } else if (abs(l.g) < 1e-3) {
    c = 1 - 2 * u.x;
  } else {
    float g = l.g;
    float s = (1 - g * g) / (1 - g + 2 * g * u.x);
    c = (1 + g * g - s * s) / (2 * g);
  }
  c = clampf(c, -1.0, 1.0);
  float sinT = safeSqrt(1 - c * c);
  float phi = 2 * kPi * u.y;
  float3 dirOut = frameFromNormal(dirIn).toWorld(float3(sinT * cos(phi), sinT * sin(phi), c));
  pdf = phaseValue(l, dirIn, dirOut);
  return dirOut;
}
