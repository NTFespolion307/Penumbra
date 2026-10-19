#pragma once

// The controller's film (docs/GUIDE.md "Disk film (.penfilm)"): running mean radiance (float32 RGB) and a
// sample count per pixel, stored per tile in a .penfilm file that doubles as the checkpoint for --resume.
//
// File layout (little-endian):
//   header (256 B) | tile index (40 B per tile) | tile blocks
//   tile block = tile width x height x { float r, g, b; uint32 count } (16 B per pixel)
// Every tile owns up to two block slots on disk. The index saved at the last checkpoint points at one of them;
// new data is only ever written to the other one, and the roles swap at the next checkpoint. So the checkpoint
// is always intact, a crash loses at most the work since it, and the file never exceeds twice the film size.
// Opening a film for resume truncates whatever was allocated after the last checkpoint.
//
// Films that fit the RAM budget keep a full copy in memory; dirty tiles reach the disk at each checkpoint.
// Disk-only films keep the tiles being worked on in memory and write a tile when it is finished (flushTile)
// or at a checkpoint.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/math/vec.h"
#include "core/render/work.h"
#include "core/system/file.h"

namespace pen {

struct FilmLayout {
  uint32_t width = 0, height = 0, tile_size = 256;
  uint32_t tiles_x = 0, tiles_y = 0;

  static FilmLayout make(uint32_t width, uint32_t height, uint32_t tileSize);
  uint32_t tileCount() const { return tiles_x * tiles_y; }
  TileRect tileRect(uint32_t tile) const;
  bool operator==(const FilmLayout&) const = default;
};

// Tile size of the film and the scheduler: the whole frame below 1080p (split by samples), else --tile-size.
uint32_t filmTileSize(uint32_t width, uint32_t height, uint32_t tileSize);

struct FilmIdentity {
  uint64_t settings_hash = 0;  // pixelSettingsHash of the render
  uint64_t scene_hash = 0;     // fingerprint of the scene files
  bool operator==(const FilmIdentity&) const = default;
};

class Film {
 public:
  // Bytes a complete film file needs on disk (one slot per tile; RAM films may use up to twice this).
  static uint64_t fileBytes(const FilmLayout& layout);
  // Creates (or overwrites) a film file.
  static std::unique_ptr<Film> create(const std::filesystem::path& path, const FilmLayout& layout,
                                      const FilmIdentity& identity, bool keepInRam);
  // Opens an existing film. Writable: for resume (drops data allocated after the last checkpoint).
  static std::unique_ptr<Film> open(const std::filesystem::path& path, bool writable, bool keepInRam);

  ~Film();
  const FilmLayout& layout() const { return layout_; }
  const FilmIdentity& identity() const { return identity_; }
  const std::filesystem::path& path() const { return file_.path(); }
  bool inRam() const { return !ram_mean_.empty(); }

  // Sample-weighted merge of a result (any rectangle) into the tiles it covers. `tileSamples` is the number of
  // samples the covered tiles hold afterwards (from the start of the render's sample range); it is stored per
  // tile so a resume knows where to continue.
  void merge(const TileResult& result, uint32_t tileSamples);
  uint32_t tileSamples(uint32_t tile) const { return index_[tile].samples; }
  bool tileHasData(uint32_t tile) const { return index_[tile].samples > 0; }
  uint32_t tilesWithData() const;
  // Disk films: the tile will receive no more samples; write it and drop it from memory.
  void flushTile(uint32_t tile);

  // Tile pixels in tile-local row-major order; zeros for tiles never rendered.
  void readTile(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const;
  // Rows [y0, y1) and columns [x0, x1) of the image into `out` (row-major, (x1 - x0) wide).
  void readRect(const TileRect& rect, std::vector<Vec3f>& out, std::vector<uint32_t>* counts = nullptr) const;

  // Makes everything merged so far durable: dirty tiles, then the tile index, then the header.
  void checkpoint();
  void close();

 private:
  struct Entry {
    uint64_t slot[2] = {0, 0};      // block offsets, 0 = not allocated
    uint64_t checksum[2] = {0, 0};
    uint32_t live = 0;              // slot holding the newest written data
    uint32_t saved = 0;             // slot the on-disk index references (never overwritten)
    uint32_t samples = 0;           // samples merged into the tile
    uint32_t written_samples = 0;   // samples in the live slot
    bool on_disk = false;           // the live slot holds data
    bool saved_valid = false;       // the on-disk index references slot[saved]
  };
  struct Tile {
    std::vector<Vec3f> mean;
    std::vector<uint32_t> count;
  };
  Film() = default;
  void writeHeader();
  void writeTile(uint32_t tile, const std::vector<Vec3f>& mean, const std::vector<uint32_t>& count);
  void readTileFromDisk(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const;
  void copyTileFromRam(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const;

  FilmLayout layout_;
  FilmIdentity identity_;
  BinaryFile file_;
  bool writable_ = false;
  std::vector<Entry> index_;
  std::vector<uint8_t> dirty_;  // tile has merged data not yet written
  uint64_t index_offset_ = 0, data_offset_ = 0, data_end_ = 0;
  std::vector<Vec3f> ram_mean_;
  std::vector<uint32_t> ram_count_;
  std::unordered_map<uint32_t, Tile> open_;  // disk films: tiles being worked on
};

}  // namespace pen
