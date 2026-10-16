#pragma once

// The controller's film (docs/GUIDE.md "Disk film (.penfilm)"): running mean radiance (float32 RGB) and a
// sample count per pixel, stored per tile in a .penfilm file that doubles as the checkpoint for --resume.
//
// File layout (little-endian):
//   header (256 B) | tile index (24 B per tile) | tile blocks, appended as tiles are merged
//   tile block = tile width x height x { float r, g, b; uint32 count } (16 B per pixel)
// A merge into a tile that already has data appends a new block (copy on write), so the index written at the
// last checkpoint always points at intact data. Opening a film for resume truncates whatever was appended
// after that checkpoint; those tiles are simply rendered again.
// Films that fit the RAM budget also keep a full copy in memory, for fast output and preview.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/math/vec.h"
#include "core/render/work.h"
#include "core/system/file.h"

namespace pen {

struct RenderSettings;

struct FilmLayout {
  uint32_t width = 0, height = 0, tile_size = 256;
  uint32_t tiles_x = 0, tiles_y = 0;

  static FilmLayout make(uint32_t width, uint32_t height, uint32_t tileSize);
  uint32_t tileCount() const { return tiles_x * tiles_y; }
  TileRect tileRect(uint32_t tile) const;
  bool operator==(const FilmLayout&) const = default;
};

// Tile size of the film and the scheduler: the whole frame below 1080p (one item), else --tile-size.
uint32_t filmTileSize(uint32_t width, uint32_t height, uint32_t tileSize);

struct FilmIdentity {
  uint64_t settings_hash = 0;  // pixelSettingsHash of the render
  uint64_t scene_hash = 0;     // fingerprint of the scene files
  bool operator==(const FilmIdentity&) const = default;
};

class Film {
 public:
  // Bytes a complete film file needs on disk.
  static uint64_t fileBytes(const FilmLayout& layout);
  // Creates (or overwrites) a film file.
  static std::unique_ptr<Film> create(const std::filesystem::path& path, const FilmLayout& layout,
                                      const FilmIdentity& identity, bool keepInRam);
  // Opens an existing film. Writable: for resume (drops data appended after the last checkpoint).
  static std::unique_ptr<Film> open(const std::filesystem::path& path, bool writable, bool keepInRam);

  ~Film();
  const FilmLayout& layout() const { return layout_; }
  const FilmIdentity& identity() const { return identity_; }
  const std::filesystem::path& path() const { return file_.path(); }
  bool inRam() const { return !ram_mean_.empty(); }

  // Sample-weighted merge of a result (any rectangle) into the tiles it covers.
  void merge(const TileResult& result);
  bool tileDone(uint32_t tile) const { return index_[tile].present; }
  uint32_t tilesDone() const;
  uint32_t tileMaxSamples(uint32_t tile) const { return index_[tile].max_samples; }

  // Tile pixels in tile-local row-major order; zeros for tiles never rendered.
  void readTile(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const;
  // Rows [y0, y1) and columns [x0, x1) of the image into `out` (row-major, (x1 - x0) wide).
  void readRect(const TileRect& rect, std::vector<Vec3f>& out, std::vector<uint32_t>* counts = nullptr) const;

  // Makes everything merged so far durable: data, then the tile index, then the header.
  void checkpoint();
  void close();

 private:
  struct Entry {
    uint64_t offset = 0;
    uint64_t checksum = 0;
    uint32_t present = 0;
    uint32_t max_samples = 0;
  };
  Film() = default;
  void writeHeader();
  void writeBlock(uint32_t tile, const std::vector<Vec3f>& mean, const std::vector<uint32_t>& count);
  void readTileFromDisk(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const;

  FilmLayout layout_;
  FilmIdentity identity_;
  BinaryFile file_;
  bool writable_ = false;
  std::vector<Entry> index_;
  uint64_t index_offset_ = 0, data_offset_ = 0, data_end_ = 0;
  std::vector<Vec3f> ram_mean_;
  std::vector<uint32_t> ram_count_;
};

}  // namespace pen
