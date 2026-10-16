#include "core/image/image.h"

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

}  // namespace pen
