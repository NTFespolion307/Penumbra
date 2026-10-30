#pragma once

#include <cstdint>
#include <vector>

#include "core/math/vec.h"
#include "core/settings/render_settings.h"

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

// The image itself or one of the AOVs (M11, DECISIONS D-127): an AOV is rendered as its own few-sample pass with
// the matching debug view (first hit), into its own film.
enum class Pass : uint32_t { Beauty = 0, Albedo = 1, Normal = 2, Depth = 3 };
inline constexpr uint32_t kPassCount = 4;
inline const char* passName(Pass p) {
  switch (p) {
    case Pass::Albedo: return "albedo";
    case Pass::Normal: return "normal";
    case Pass::Depth: return "depth";
    default: return "beauty";
  }
}
// The integrator a pass renders: the render's own for the image, the matching debug view for an AOV.
inline Integrator passIntegrator(Pass p, Integrator beauty) {
  switch (p) {
    case Pass::Albedo: return Integrator::DebugAlbedo;
    case Pass::Normal: return Integrator::DebugNormal;
    case Pass::Depth: return Integrator::DebugDepth;
    default: return beauty;
  }
}

// A film tile (clipped to --region) plus one chunk of its samples [sample_begin, sample_end)
// (docs/GUIDE.md "Work item"). Chunk boundaries depend only on the settings (DECISIONS D-045).
struct WorkItem {
  uint64_t id = 0;  // (pass << 56) | (tile << 32) | chunk
  uint32_t tile = 0;
  uint32_t chunk = 0;
  Pass pass = Pass::Beauty;
  TileRect rect;
  uint32_t sample_begin = 0, sample_end = 0;
  LeaseId lease = 0;  // set when the item is handed to a device
  uint32_t samples() const { return sample_end - sample_begin; }

  static uint64_t makeId(uint32_t tile, uint32_t chunk, Pass pass = Pass::Beauty) {
    return (uint64_t(pass) << 56) | (uint64_t(tile) << 32) | chunk;
  }
  // The parts of an id (tiles stay below 2^24).
  static uint32_t idTile(uint64_t id) { return static_cast<uint32_t>((id >> 32) & 0xffffffu); }
  static uint32_t idChunk(uint64_t id) { return static_cast<uint32_t>(id & 0xffffffffu); }
  static Pass idPass(uint64_t id) { return static_cast<Pass>((id >> 56) & 0xffu); }
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
