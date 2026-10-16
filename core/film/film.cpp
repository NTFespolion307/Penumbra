#include "core/film/film.h"

namespace pen {

Film::Film(uint32_t width, uint32_t height)
    : width_(width), height_(height), mean_(size_t(width) * height), count_(size_t(width) * height, 0) {}

void Film::merge(const TileResult& r) {
  const uint32_t k = r.sample_end - r.sample_begin;
  if (k == 0) return;
  const uint32_t w = r.rect.width();
  for (uint32_t y = r.rect.y0; y < r.rect.y1; ++y) {
    for (uint32_t x = r.rect.x0; x < r.rect.x1; ++x) {
      const size_t i = size_t(y) * width_ + x;
      const Vec3f& v = r.mean[size_t(y - r.rect.y0) * w + (x - r.rect.x0)];
      const uint32_t n = count_[i];
      if (n == 0) {
        mean_[i] = v;  // exact: a single result is stored bit for bit
      } else {
        const float t = static_cast<float>(k) / static_cast<float>(uint64_t(n) + k);
        mean_[i] = mean_[i] + (v - mean_[i]) * t;
      }
      count_[i] = n + k;
    }
  }
}

}  // namespace pen
