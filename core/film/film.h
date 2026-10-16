#pragma once

#include <cstdint>
#include <vector>

#include "core/math/vec.h"
#include "core/render/work.h"

namespace pen {

// The controller's accumulation buffer in RAM: running mean radiance (float32) and sample count per pixel.
// Disk films (.penfilm) arrive in M2.
class Film {
 public:
  Film() = default;
  Film(uint32_t width, uint32_t height);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  // Sample-weighted merge of a tile result into the film.
  void merge(const TileResult& result);

  const Vec3f& mean(uint32_t x, uint32_t y) const { return mean_[size_t(y) * width_ + x]; }
  uint32_t samples(uint32_t x, uint32_t y) const { return count_[size_t(y) * width_ + x]; }
  const std::vector<Vec3f>& means() const { return mean_; }
  uint64_t bytes() const { return mean_.size() * (sizeof(Vec3f) + sizeof(uint32_t)); }

 private:
  uint32_t width_ = 0, height_ = 0;
  std::vector<Vec3f> mean_;
  std::vector<uint32_t> count_;
};

}  // namespace pen
