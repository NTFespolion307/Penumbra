#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/vec.h"

namespace pen {

// Linear float RGB image (renders, diffs, environment maps).
struct ImageRGB {
  uint32_t width = 0, height = 0;
  std::vector<Vec3f> pixels;  // row-major, row 0 at the top
  Vec3f& at(uint32_t x, uint32_t y) { return pixels[size_t(y) * width + x]; }
  const Vec3f& at(uint32_t x, uint32_t y) const { return pixels[size_t(y) * width + x]; }
};

enum class TextureWrap : uint8_t { Repeat, Clamp, Mirror };

// A material texture: 8-bit RGBA for ordinary images (decoded from sRGB at lookup when the glTF slot is a
// colour), or float RGBA for HDR images.
struct Texture {
  std::string name;
  uint32_t width = 0, height = 0;
  bool is_float = false;
  std::vector<uint8_t> ldr;  // RGBA8
  std::vector<float> hdr;    // RGBA32F
  TextureWrap wrap_s = TextureWrap::Repeat, wrap_t = TextureWrap::Repeat;
  bool nearest = false;

  // Bilinear (or nearest) lookup at uv with glTF conventions (uv (0,0) = top-left of the image).
  Vec4f sample(Vec2f uv, bool srgb) const;
  size_t bytes() const { return ldr.size() + hdr.size() * sizeof(float); }

 private:
  Vec4f texel(int x, int y, bool srgb) const;
};

// The texture halved `halvings` times (each side max(1, n / 2)), every texel the mean of the 2x2 texels it covers,
// in the stored values: memory tier 2 (docs/GUIDE.md "Scene placement ladder"). The GPU backends and the CPU use
// this same function, so a tier 2 render can be checked against the CPU.
Texture downsized(const Texture& t, uint32_t halvings);

float srgbToLinear(float c);
// srgbToLinear(i / 255) for i = 0..255, as Texture lookups use it (also for GPU backends).
const float* srgbToLinearTable();
float linearToSrgb(float c);

}  // namespace pen
