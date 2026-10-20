// Port of core/math/vec.h and the Affine3f helpers of core/math/transform.h.
#pragma once

static const float kPi = 3.14159265358979323846;
static const float kInvPi = 0.31830988618379067154;
static const float kTwoPi = 6.28318530717958647692;
static const float kInv2Pi = 0.15915494309189533577;
static const float kInv4Pi = 0.07957747154594766788;
static const float kInfinity = 1.0 / 0.0;

float sqr(float v) { return v * v; }
float safeSqrt(float v) { return sqrt(v > 0 ? v : 0); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a * (1 - t) + b * t; }
float3 lerp3(float3 a, float3 b, float t) { return a * (1 - t) + b * t; }
float maxComponent(float3 a) { return a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z); }
float luminance(float3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
bool isBlack(float3 c) { return c.x == 0 && c.y == 0 && c.z == 0; }
bool isFiniteF(float v) { return !isinf(v) && !isnan(v); }
bool isFinite3(float3 c) { return isFiniteF(c.x) && isFiniteF(c.y) && isFiniteF(c.z); }
float lengthSquared(float3 a) { return dot(a, a); }
float3 normalizeSafe(float3 a) {
  float l2 = dot(a, a);
  return l2 > 0 ? a * (1.0 / sqrt(l2)) : float3(0, 0, 1);
}

// Orthonormal basis around a unit normal (Duff et al. 2017), as core/math/vec.h Frame.
struct Frame {
  float3 t;
  float3 b;
  float3 n;
  float3 toLocal(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
  float3 toWorld(float3 v) { return t * v.x + b * v.y + n * v.z; }
};

Frame frameFromNormal(float3 n) {
  Frame f;
  f.n = n;
  float sign = n.z >= 0 ? 1.0 : -1.0;  // copysign(1, n.z), with -0 treated as +0 like the CPU on these inputs
  if (asuint(n.z) == 0x80000000u) sign = -1.0;
  float a = -1.0 / (sign + n.z);
  float bb = n.x * n.y * a;
  f.t = float3(1.0 + sign * n.x * n.x * a, sign * bb, -sign * n.x);
  f.b = float3(bb, sign + n.y * n.y * a, -n.y);
  return f;
}

Frame frameFromNormalTangent(float3 normal, float3 tangent) {
  float3 tt = tangent - normal * dot(normal, tangent);
  if (lengthSquared(tt) < 1e-12) return frameFromNormal(normal);
  Frame f;
  f.n = normal;
  f.t = normalizeSafe(tt);
  f.b = cross(f.n, f.t);
  return f;
}

// Row-major 3x4 affine transforms stored as float[12].
float3 xformPoint(float m[12], float3 p) {
  return float3(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
}
float3 xformVector(float m[12], float3 v) {
  return float3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
                m[8] * v.x + m[9] * v.y + m[10] * v.z);
}
// Normal transform: the transpose of the inverse's linear part (inv given).
float3 xformNormal(float inv[12], float3 n) {
  return float3(inv[0] * n.x + inv[4] * n.y + inv[8] * n.z, inv[1] * n.x + inv[5] * n.y + inv[9] * n.z,
                inv[2] * n.x + inv[6] * n.y + inv[10] * n.z);
}
