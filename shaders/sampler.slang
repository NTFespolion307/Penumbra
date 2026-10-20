// Port of core/sampling/sampler.h: Owen-scrambled Sobol. Integer arithmetic only, so every device draws the
// same numbers as the CPU, bit for bit.
#pragma once

uint hashU32(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

uint hashCombine(uint seed, uint v) { return hashU32(seed ^ (v + 0x9e3779b9u + (seed << 6) + (seed >> 2))); }

uint laineKarras(uint x, uint seed) {
  x ^= x * 0x3d20adeau;
  x += seed;
  x *= (seed >> 16) | 1u;
  x ^= x * 0x05526c56u;
  x ^= x * 0x53a22864u;
  return x;
}

uint nestedUniformScramble(uint x, uint seed) { return reversebits(laineKarras(reversebits(x), seed)); }

uint sobol01(uint index, int dim) {
  if (dim == 0) return reversebits(index);
  uint result = 0;
  uint v = 0x80000000u;
  for (; index != 0; index >>= 1, v ^= v >> 1)
    if ((index & 1u) != 0) result ^= v;
  return result;
}

float toUnitFloat(uint x) { return float(x >> 8) * (1.0 / 16777216.0); }

float2 sobolOwen2D(uint index, uint seed) {
  uint shuffled = nestedUniformScramble(index, hashU32(seed));
  uint x = nestedUniformScramble(sobol01(shuffled, 0), hashCombine(seed, 0xa511e9b3u));
  uint y = nestedUniformScramble(sobol01(shuffled, 1), hashCombine(seed, 0x63d83595u));
  return float2(toUnitFloat(x), toUnitFloat(y));
}

struct PathSampler {
  uint pixel_seed;
  uint index;
  uint dim;

  [mutating] float2 next2D() { return sobolOwen2D(index, hashCombine(pixel_seed, dim++)); }
  [mutating] float next1D() { return next2D().x; }
  [mutating] void setDimension(uint d) { dim = d; }
};

PathSampler makeSampler(uint px, uint py, uint sampleIndex, uint seed) {
  PathSampler s;
  s.pixel_seed = hashCombine(hashCombine(hashU32(px), py), hashU32(seed ^ 0x2545f491u));
  s.index = sampleIndex;
  s.dim = 0;
  return s;
}
