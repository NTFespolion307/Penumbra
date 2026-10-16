#pragma once

// Small vector math shared by the CPU renderer and, later, GPU code. Header-only, no exceptions, no STL
// containers, so the same code can compile for CUDA (PEN_HD becomes __host__ __device__ there).

#include <cmath>
#include <cstdint>

#ifndef PEN_HD
#define PEN_HD
#endif

namespace pen {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kInvPi = 0.31830988618379067154f;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kInv2Pi = 0.15915494309189533577f;
inline constexpr float kInv4Pi = 0.07957747154594766788f;

struct Vec2f {
  float x = 0, y = 0;
  PEN_HD constexpr Vec2f() = default;
  PEN_HD constexpr Vec2f(float x_, float y_) : x(x_), y(y_) {}
  PEN_HD constexpr float operator[](int i) const { return i == 0 ? x : y; }
};

struct Vec3f {
  float x = 0, y = 0, z = 0;
  PEN_HD constexpr Vec3f() = default;
  PEN_HD constexpr explicit Vec3f(float s) : x(s), y(s), z(s) {}
  PEN_HD constexpr Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  PEN_HD constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  PEN_HD float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
  PEN_HD constexpr bool operator==(const Vec3f&) const = default;
};

struct Vec4f {
  float x = 0, y = 0, z = 0, w = 0;
  PEN_HD constexpr Vec4f() = default;
  PEN_HD constexpr Vec4f(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  PEN_HD constexpr Vec3f xyz() const { return {x, y, z}; }
};

// ---- Vec2f ----
PEN_HD constexpr Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
PEN_HD constexpr Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
PEN_HD constexpr Vec2f operator*(Vec2f a, float s) { return {a.x * s, a.y * s}; }
PEN_HD constexpr Vec2f operator*(float s, Vec2f a) { return {a.x * s, a.y * s}; }

// ---- Vec3f ----
PEN_HD constexpr Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
PEN_HD constexpr Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
PEN_HD constexpr Vec3f operator-(Vec3f a) { return {-a.x, -a.y, -a.z}; }
PEN_HD constexpr Vec3f operator*(Vec3f a, Vec3f b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
PEN_HD constexpr Vec3f operator*(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
PEN_HD constexpr Vec3f operator*(float s, Vec3f a) { return {a.x * s, a.y * s, a.z * s}; }
PEN_HD constexpr Vec3f operator/(Vec3f a, float s) { return a * (1.0f / s); }
PEN_HD constexpr Vec3f operator/(Vec3f a, Vec3f b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
PEN_HD inline Vec3f& operator+=(Vec3f& a, Vec3f b) { return a = a + b; }
PEN_HD inline Vec3f& operator-=(Vec3f& a, Vec3f b) { return a = a - b; }
PEN_HD inline Vec3f& operator*=(Vec3f& a, Vec3f b) { return a = a * b; }
PEN_HD inline Vec3f& operator*=(Vec3f& a, float s) { return a = a * s; }

PEN_HD constexpr float dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
PEN_HD constexpr Vec3f cross(Vec3f a, Vec3f b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
PEN_HD inline float length(Vec3f a) { return std::sqrt(dot(a, a)); }
PEN_HD constexpr float lengthSquared(Vec3f a) { return dot(a, a); }
PEN_HD inline Vec3f normalize(Vec3f a) {
  const float l2 = dot(a, a);
  return l2 > 0 ? a * (1.0f / std::sqrt(l2)) : Vec3f(0, 0, 1);
}
PEN_HD inline Vec3f vmin(Vec3f a, Vec3f b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
PEN_HD inline Vec3f vmax(Vec3f a, Vec3f b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }
PEN_HD inline Vec3f vabs(Vec3f a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
PEN_HD constexpr float maxComponent(Vec3f a) { return a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z); }
PEN_HD constexpr float luminance(Vec3f c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
PEN_HD constexpr bool isBlack(Vec3f c) { return c.x == 0 && c.y == 0 && c.z == 0; }
PEN_HD inline bool isFinite(Vec3f c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }
PEN_HD constexpr Vec3f lerp(Vec3f a, Vec3f b, float t) { return a * (1 - t) + b * t; }

PEN_HD constexpr float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
PEN_HD constexpr float lerp(float a, float b, float t) { return a * (1 - t) + b * t; }
PEN_HD constexpr float sqr(float v) { return v * v; }
PEN_HD inline float safeSqrt(float v) { return std::sqrt(v > 0 ? v : 0); }

// Orthonormal basis around a unit normal (Duff et al. 2017, branchless).
struct Frame {
  Vec3f t, b, n;
  PEN_HD Frame() = default;
  PEN_HD explicit Frame(Vec3f normal) : n(normal) {
    const float sign = std::copysign(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float bb = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x};
    b = {bb, sign + n.y * n.y * a, -n.y};
  }
  // From a normal and a (not necessarily orthogonal) tangent: Gram-Schmidt.
  PEN_HD static Frame fromNormalTangent(Vec3f normal, Vec3f tangent) {
    Frame f;
    f.n = normal;
    Vec3f tt = tangent - normal * dot(normal, tangent);
    if (lengthSquared(tt) < 1e-12f) return Frame(normal);
    f.t = normalize(tt);
    f.b = cross(f.n, f.t);
    return f;
  }
  PEN_HD Vec3f toLocal(Vec3f v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
  PEN_HD Vec3f toWorld(Vec3f v) const { return t * v.x + b * v.y + n * v.z; }
};

}  // namespace pen
