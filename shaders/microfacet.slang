// Port of core/shading/microfacet.h.
#pragma once
#include "math.slang"

float absCosTheta(float3 w) { return abs(w.z); }
bool sameHemisphere(float3 a, float3 b) { return a.z * b.z > 0; }
float3 reflectDir(float3 wo, float3 n) { return -wo + n * (2 * dot(wo, n)); }

bool refractDir(float3 wi, float3 n, float eta, out float etap, out float3 wt) {
  etap = 1;
  wt = float3(0, 0, 0);
  float cosI = dot(n, wi);
  if (cosI < 0) {
    eta = 1 / eta;
    cosI = -cosI;
    n = -n;
  }
  float sin2I = max(0.0, 1 - cosI * cosI);
  float sin2T = sin2I / (eta * eta);
  if (sin2T >= 1) return false;
  float cosT = safeSqrt(1 - sin2T);
  wt = -wi / eta + n * (cosI / eta - cosT);
  etap = eta;
  return true;
}

float fresnelDielectric(float cosI, float eta) {
  cosI = clampf(cosI, -1.0, 1.0);
  if (cosI < 0) {
    eta = 1 / eta;
    cosI = -cosI;
  }
  float sin2T = (1 - cosI * cosI) / (eta * eta);
  if (sin2T >= 1) return 1.0;
  float cosT = safeSqrt(1 - sin2T);
  float rParl = (eta * cosI - cosT) / (eta * cosI + cosT);
  float rPerp = (cosI - eta * cosT) / (cosI + eta * cosT);
  return 0.5 * (rParl * rParl + rPerp * rPerp);
}

float schlickWeight(float cosTheta) {
  float m = clampf(1 - cosTheta, 0.0, 1.0);
  float m2 = m * m;
  return m2 * m2 * m;
}
float3 fresnelSchlick3(float3 f0, float cosTheta) { return f0 + (float3(1, 1, 1) - f0) * schlickWeight(cosTheta); }
float fresnelSchlick1(float f0, float cosTheta) { return f0 + (1 - f0) * schlickWeight(cosTheta); }

static const float kDeltaAlpha = 1e-3;

struct GGX {
  float ax;
  float ay;

  bool isDelta() { return ax < kDeltaAlpha && ay < kDeltaAlpha; }

  float D(float3 h) {
    if (h.z <= 0) return 0;
    float e = sqr(h.x / ax) + sqr(h.y / ay) + sqr(h.z);
    return 1.0 / (kPi * ax * ay * e * e);
  }
  float lambda(float3 w) {
    if (w.z == 0) return 1e30;
    float a2 = (sqr(w.x * ax) + sqr(w.y * ay)) / sqr(w.z);
    return 0.5 * (sqrt(1 + a2) - 1);
  }
  float G1(float3 w) { return 1 / (1 + lambda(w)); }
  float G(float3 wo, float3 wi) { return 1 / (1 + lambda(wo) + lambda(wi)); }
  float Dvisible(float3 w, float3 h) {
    float c = absCosTheta(w);
    return c > 0 ? G1(w) / c * D(h) * abs(dot(w, h)) : 0;
  }
  float3 sampleVisible(float3 w, float2 u) {
    float3 wh = normalizeSafe(float3(ax * w.x, ay * w.y, w.z));
    if (wh.z < 0) wh = -wh;
    float lensq = wh.x * wh.x + wh.y * wh.y;
    float3 t1 = lensq > 0 ? float3(-wh.y, wh.x, 0) / sqrt(lensq) : float3(1, 0, 0);
    float3 t2 = cross(wh, t1);
    float r = sqrt(u.x), phi = kTwoPi * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1 + wh.z);
    p2 = (1 - s) * safeSqrt(1 - p1 * p1) + s * p2;
    float3 nh = t1 * p1 + t2 * p2 + wh * safeSqrt(1 - p1 * p1 - p2 * p2);
    return normalizeSafe(float3(ax * nh.x, ay * nh.y, max(1e-6, nh.z)));
  }
};

GGX makeGGX(float ax, float ay) {
  GGX g;
  g.ax = ax;
  g.ay = ay;
  return g;
}
