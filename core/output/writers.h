#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "core/image/image_io.h"
#include "core/settings/render_settings.h"

namespace pen {

// Streams an image to disk in horizontal bands from top to bottom (docs/GUIDE.md "Format limits": never the
// whole image in memory, except WebP, which is capped at 16,383 px per side).
class ImageWriter {
 public:
  virtual ~ImageWriter() = default;
  // Linear radiance (EXR, 32-bit TIFF) or display-encoded values in [0,1] (everything else).
  virtual bool wantsLinear() const = 0;
  // Rows y0 .. y0 + rows - 1 as interleaved RGB floats, `width` pixels per row. Bands arrive in order and,
  // except for the last, are kBandRows tall.
  virtual void writeBand(uint32_t y0, uint32_t rows, std::span<const float> rgb) = 0;
  virtual void finish() = 0;

  static constexpr uint32_t kBandRows = 256;
};

std::unique_ptr<ImageWriter> makeImageWriter(const std::filesystem::path& path, ImageFormat format, uint32_t bitDepth,
                                             uint32_t width, uint32_t height, const ImageMetadata& metadata);

// Upper bound of the output file size (uncompressed pixel data), for the free-disk check.
uint64_t estimateOutputBytes(ImageFormat format, uint32_t bitDepth, uint32_t width, uint32_t height);

}  // namespace pen
