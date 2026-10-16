#pragma once

// Deterministic sampling (docs/GUIDE.md "Deterministic sampling"): every random number is a pure function of
// (pixel x, pixel y, sample index, dimension, seed). Any device can render any tile and sample range and
// draw exactly the same numbers.
//
// Owen-scrambled, shuffled Sobol (0,2)-sequence per dimension pair, after Burley, "Practical Hash-based Owen
// Scrambling" (JCGT 2020), with the improved Laine-Karras hash by Vegdahl (2021).

#include <cstdint>

#include "core/math/vec.h"

namespace pen {

// lowbias32 integer hash (C. Wellons).
PEN_HD constexpr uint32_t hashU32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

PEN_HD constexpr uint32_t hashCombine(uint32_t seed, uint32_t v) { return hashU32(seed ^ (v + 0x9e3779b9u + (seed << 6) + (seed >> 2))); }

PEN_HD constexpr uint32_t reverseBits(uint32_t x) {
  x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
  x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
  x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
  x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
  return (x >> 16) | (x << 16);
}

PEN_HD constexpr uint32_t laineKarras(uint32_t x, uint32_t seed) {
  x ^= x * 0x3d20adeau;
  x += seed;
  x *= (seed >> 16) | 1u;
  x ^= x * 0x05526c56u;
  x ^= x * 0x53a22864u;
  return x;
}

PEN_HD constexpr uint32_t nestedUniformScramble(uint32_t x, uint32_t seed) {
  return reverseBits(laineKarras(reverseBits(x), seed));
}

// First two Sobol dimensions. Dimension 0 is the van der Corput sequence; dimension 1 uses the direction
// numbers of the primitive polynomial x + 1: v0 = 2^31, v_k = v_{k-1} ^ (v_{k-1} >> 1).
PEN_HD constexpr uint32_t sobol01(uint32_t index, int dim) {
  if (dim == 0) return reverseBits(index);
  uint32_t result = 0;
  uint32_t v = 0x80000000u;
  for (; index != 0; index >>= 1, v ^= v >> 1)
    if (index & 1u) result ^= v;
  return result;
}

PEN_HD constexpr float toUnitFloat(uint32_t x) { return static_cast<float>(x >> 8) * (1.0f / 16777216.0f); }

// Owen-scrambled 2D Sobol point `index` for the given seed.
PEN_HD constexpr Vec2f sobolOwen2D(uint32_t index, uint32_t seed) {
  const uint32_t shuffled = nestedUniformScramble(index, hashU32(seed));
  const uint32_t x = nestedUniformScramble(sobol01(shuffled, 0), hashCombine(seed, 0xa511e9b3u));
  const uint32_t y = nestedUniformScramble(sobol01(shuffled, 1), hashCombine(seed, 0x63d83595u));
  return {toUnitFloat(x), toUnitFloat(y)};
}

// Per-pixel, per-sample stream of dimensions. Every call to next1D or next2D consumes one dimension pair, so
// the dimension layout depends only on the order of calls, which the integrator keeps fixed per bounce.
class Sampler {
 public:
  PEN_HD Sampler(uint32_t px, uint32_t py, uint32_t sampleIndex, uint32_t seed)
      : pixel_seed_(hashCombine(hashCombine(hashU32(px), py), hashU32(seed ^ 0x2545f491u))),
        index_(sampleIndex) {}

  PEN_HD Vec2f next2D() { return sobolOwen2D(index_, hashCombine(pixel_seed_, dim_++)); }
  PEN_HD float next1D() { return next2D().x; }

  // Jump to a fixed dimension, so each bounce starts at a known offset regardless of earlier branches.
  PEN_HD void setDimension(uint32_t dim) { dim_ = dim; }
  PEN_HD uint32_t dimension() const { return dim_; }

 private:
  uint32_t pixel_seed_;
  uint32_t index_;
  uint32_t dim_ = 0;
};

}  // namespace pen
