#pragma once

#include "core/math/vec.h"

namespace pen {

// Affine transform stored as a 3x4 row-major matrix: p' = M[0..2][0..2] * p + M[.][3].
struct Affine3f {
  float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

  PEN_HD static Affine3f identity() { return {}; }

  PEN_HD static Affine3f translate(Vec3f t) {
    Affine3f a;
    a.m[0][3] = t.x;
    a.m[1][3] = t.y;
    a.m[2][3] = t.z;
    return a;
  }

  PEN_HD static Affine3f scale(Vec3f s) {
    Affine3f a;
    a.m[0][0] = s.x;
    a.m[1][1] = s.y;
    a.m[2][2] = s.z;
    return a;
  }

  // Unit quaternion (x, y, z, w) as in glTF.
  PEN_HD static Affine3f rotate(Vec4f q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Affine3f a;
    a.m[0][0] = 1 - 2 * (y * y + z * z);
    a.m[0][1] = 2 * (x * y - z * w);
    a.m[0][2] = 2 * (x * z + y * w);
    a.m[1][0] = 2 * (x * y + z * w);
    a.m[1][1] = 1 - 2 * (x * x + z * z);
    a.m[1][2] = 2 * (y * z - x * w);
    a.m[2][0] = 2 * (x * z - y * w);
    a.m[2][1] = 2 * (y * z + x * w);
    a.m[2][2] = 1 - 2 * (x * x + y * y);
    return a;
  }

  // From a glTF column-major 4x4 matrix.
  PEN_HD static Affine3f fromColumnMajor(const float* c) {
    Affine3f a;
    for (int r = 0; r < 3; ++r)
      for (int col = 0; col < 4; ++col) a.m[r][col] = c[col * 4 + r];
    return a;
  }

  PEN_HD Vec3f point(Vec3f p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
  }
  PEN_HD Vec3f vector(Vec3f v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
  }
  // Transforms a normal with the inverse transpose of the linear part (unnormalized).
  PEN_HD Vec3f normal(Vec3f n) const {
    const Affine3f inv = inverse();
    return {inv.m[0][0] * n.x + inv.m[1][0] * n.y + inv.m[2][0] * n.z,
            inv.m[0][1] * n.x + inv.m[1][1] * n.y + inv.m[2][1] * n.z,
            inv.m[0][2] * n.x + inv.m[1][2] * n.y + inv.m[2][2] * n.z};
  }

  PEN_HD float determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  }

  PEN_HD Affine3f inverse() const {
    const float det = determinant();
    const float id = det != 0 ? 1.0f / det : 0.0f;
    Affine3f r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    const Vec3f t{m[0][3], m[1][3], m[2][3]};
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * t.x + r.m[i][1] * t.y + r.m[i][2] * t.z);
    return r;
  }
};

PEN_HD inline Affine3f operator*(const Affine3f& a, const Affine3f& b) {
  Affine3f r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
      if (j == 3) v += a.m[i][3];
      r.m[i][j] = v;
    }
  }
  return r;
}

struct Bounds3f {
  Vec3f lo{1e30f, 1e30f, 1e30f};
  Vec3f hi{-1e30f, -1e30f, -1e30f};
  PEN_HD void extend(Vec3f p) {
    lo = vmin(lo, p);
    hi = vmax(hi, p);
  }
  PEN_HD void extend(const Bounds3f& b) {
    lo = vmin(lo, b.lo);
    hi = vmax(hi, b.hi);
  }
  PEN_HD bool valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
  PEN_HD Vec3f center() const { return (lo + hi) * 0.5f; }
  PEN_HD Vec3f extent() const { return hi - lo; }
};

}  // namespace pen
