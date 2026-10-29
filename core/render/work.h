#pragma once

#include <cstdint>
#include <vector>

#include "core/math/vec.h"

namespace pen {

// Half-open pixel rectangle [x0, x1) x [y0, y1).
struct TileRect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t width() const { return x1 - x0; }
  uint32_t height() const { return y1 - y0; }
  uint64_t pixels() const { return uint64_t(width()) * height(); }
  bool operator==(const TileRect&) const = default;
};

using LeaseId = uint64_t;

// A film tile (clipped to --region) plus one chunk of its samples [sample_begin, sample_end)
// (docs/GUIDE.md "Work item"). Chunk boundaries depend only on the settings (DECISIONS D-045).
struct WorkItem {
  uint64_t id = 0;  // (tile << 32) | chunk
  uint32_t tile = 0;
  uint32_t chunk = 0;
  TileRect rect;
  uint32_t sample_begin = 0, sample_end = 0;
  LeaseId lease = 0;  // set when the item is handed to a device
  uint32_t samples() const { return sample_end - sample_begin; }

  static uint64_t makeId(uint32_t tile, uint32_t chunk) { return (uint64_t(tile) << 32) | chunk; }
};

// What a device sends back for a lease: the mean radiance per pixel over the item's samples.
struct TileResult {
  uint64_t item_id = 0;
  LeaseId lease = 0;
  uint32_t device = 0;
  TileRect rect;
  uint32_t sample_begin = 0, sample_end = 0;
  std::vector<Vec3f> mean;   // row-major over rect
  uint32_t nan_count = 0;    // non-finite samples replaced by 0
  double render_seconds = 0;
  double decode_seconds = 0;  // controller side: unpacking it from the network (M9 throughput, D-112)
  bool cancelled = false;    // stopped early by IBackend::cancel(); the pixels are incomplete and never merged
};

}  // namespace pen
