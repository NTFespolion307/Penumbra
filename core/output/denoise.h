#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/math/vec.h"

namespace pen {

// Intel Open Image Denoise (M11, DECISIONS D-128): the "RT" filter in HDR mode on scene-linear radiance, guided by
// the albedo and normal AOVs. One per output; OIDN picks its device (CUDA on an NVIDIA GPU, else the CPU), and
// describe() names it for the log. Available only in builds with PEN_ENABLE_OIDN (denoiserAvailable()).
class OidnDenoiser {
 public:
  OidnDenoiser();
  ~OidnDenoiser();
  std::string describe() const;
  // For manual tiling: tile sizes and offsets in multiples of tileAlignment, tiles overlapping by tileOverlap.
  uint32_t tileAlignment() const;
  uint32_t tileOverlap() const;

  // Denoises a w x h image in place. albedo and normal are the AOVs for the same pixels (normals in [-1, 1]).
  // inputScale: one value for every tile of an image (OIDN's own, per call, would differ between tiles and make
  // seams); see autoExposure.
  void run(std::vector<Vec3f>& color, const std::vector<Vec3f>& albedo, const std::vector<Vec3f>& normal, uint32_t w,
           uint32_t h, float inputScale);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool denoiserAvailable();

// OIDN's automatic input scale for HDR: 0.18 over the geometric mean luminance of `pixels` (1 if all black).
float autoExposure(const std::vector<Vec3f>& pixels);

}  // namespace pen
