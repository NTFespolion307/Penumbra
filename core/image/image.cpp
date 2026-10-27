#include "core/image/image.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pen {

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float linearToSrgb(float c) {
  if (c <= 0.0f) return 0.0f;
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

namespace {

const std::array<float, 256>& srgbLut() {
  static const std::array<float, 256> lut = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = srgbToLinear(i / 255.0f);
    return t;
  }();
  return lut;
}

int wrapCoord(int i, int n, TextureWrap mode) {
  switch (mode) {
    case TextureWrap::Clamp: return i < 0 ? 0 : (i >= n ? n - 1 : i);
    case TextureWrap::Mirror: {
      const int period = 2 * n;
      int m = i % period;
      if (m < 0) m += period;
      return m < n ? m : period - 1 - m;
    }
    case TextureWrap::Repeat:
    default: {
      int m = i % n;
      return m < 0 ? m + n : m;
    }
  }
}

}  // namespace

const float* srgbToLinearTable() { return srgbLut().data(); }

Vec4f Texture::texel(int x, int y, bool srgb) const {
  x = wrapCoord(x, static_cast<int>(width), wrap_s);
  y = wrapCoord(y, static_cast<int>(height), wrap_t);
  const size_t i = (size_t(y) * width + size_t(x)) * 4;
  if (is_float) return {hdr[i], hdr[i + 1], hdr[i + 2], hdr[i + 3]};
  const auto& lut = srgbLut();
  if (srgb) return {lut[ldr[i]], lut[ldr[i + 1]], lut[ldr[i + 2]], ldr[i + 3] / 255.0f};
  return {ldr[i] / 255.0f, ldr[i + 1] / 255.0f, ldr[i + 2] / 255.0f, ldr[i + 3] / 255.0f};
}

Vec4f Texture::sample(Vec2f uv, bool srgb) const {
  if (width == 0 || height == 0) return {1, 1, 1, 1};
  const float fx = uv.x * width - 0.5f, fy = uv.y * height - 0.5f;
  if (nearest) return texel(static_cast<int>(std::floor(fx + 0.5f)), static_cast<int>(std::floor(fy + 0.5f)), srgb);
  const float x0f = std::floor(fx), y0f = std::floor(fy);
  const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
  const float tx = fx - x0f, ty = fy - y0f;
  const Vec4f a = texel(x0, y0, srgb), b = texel(x0 + 1, y0, srgb);
  const Vec4f c = texel(x0, y0 + 1, srgb), d = texel(x0 + 1, y0 + 1, srgb);
  const auto mix = [](Vec4f p, Vec4f q, float t) {
    return Vec4f{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t};
  };
  return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

Texture downsized(const Texture& t, uint32_t halvings) {
  Texture cur = t;
  for (uint32_t h = 0; h < halvings && (cur.width > 1 || cur.height > 1); ++h) {
    Texture next;
    next.name = cur.name;
    next.is_float = cur.is_float;
    next.wrap_s = cur.wrap_s;
    next.wrap_t = cur.wrap_t;
    next.nearest = cur.nearest;
    next.width = std::max(1u, cur.width / 2);
    next.height = std::max(1u, cur.height / 2);
    const size_t n = size_t(next.width) * next.height * 4;
    if (cur.is_float) next.hdr.resize(n);
    else next.ldr.resize(n);
    for (uint32_t y = 0; y < next.height; ++y) {
      const uint32_t y0 = std::min(2 * y, cur.height - 1), y1 = std::min(2 * y + 1, cur.height - 1);
      for (uint32_t x = 0; x < next.width; ++x) {
        const uint32_t x0 = std::min(2 * x, cur.width - 1), x1 = std::min(2 * x + 1, cur.width - 1);
        const size_t a = (size_t(y0) * cur.width + x0) * 4, b = (size_t(y0) * cur.width + x1) * 4;
        const size_t c = (size_t(y1) * cur.width + x0) * 4, d = (size_t(y1) * cur.width + x1) * 4;
        const size_t o = (size_t(y) * next.width + x) * 4;
        for (int k = 0; k < 4; ++k) {
          if (cur.is_float)
            next.hdr[o + k] = (cur.hdr[a + k] + cur.hdr[b + k] + cur.hdr[c + k] + cur.hdr[d + k]) * 0.25f;
          else
            next.ldr[o + k] = static_cast<uint8_t>(
                (unsigned(cur.ldr[a + k]) + cur.ldr[b + k] + cur.ldr[c + k] + cur.ldr[d + k] + 2) / 4);
        }
      }
    }
    cur = std::move(next);
  }
  return cur;
}

}  // namespace pen
