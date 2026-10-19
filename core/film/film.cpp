#include "core/film/film.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

#include "core/system/hash.h"
#include "core/system/paths.h"
#include "core/version.h"

namespace pen {

namespace {

constexpr char kMagic[8] = {'P', 'E', 'N', 'F', 'I', 'L', 'M', '1'};
constexpr uint32_t kVersion = 2;
constexpr uint64_t kHeaderBytes = 256;
constexpr uint64_t kEntryBytes = 40;
constexpr uint64_t kPixelBytes = 16;
constexpr uint32_t kFlagPresent = 1, kFlagSavedSlot1 = 2;

// Fixed header; every field little-endian (x86-64 and ARM64 hosts both are).
struct Header {
  char magic[8];
  uint32_t version;
  uint32_t header_bytes;
  uint32_t width, height, tile_size, tiles_x, tiles_y;
  uint32_t reserved0;
  uint64_t index_offset, data_offset, data_end;
  uint64_t settings_hash, scene_hash;
  char app_version[32];
  char git_hash[32];
};
static_assert(sizeof(Header) <= kHeaderBytes);

void copyText(char* dst, size_t n, std::string_view src) {
  std::memset(dst, 0, n);
  std::memcpy(dst, src.data(), std::min(n - 1, src.size()));
}

std::vector<uint8_t> packBlock(const std::vector<Vec3f>& mean, const std::vector<uint32_t>& count) {
  std::vector<uint8_t> bytes(mean.size() * kPixelBytes);
  for (size_t i = 0; i < mean.size(); ++i) {
    uint8_t* p = bytes.data() + i * kPixelBytes;
    std::memcpy(p, &mean[i].x, 4);
    std::memcpy(p + 4, &mean[i].y, 4);
    std::memcpy(p + 8, &mean[i].z, 4);
    std::memcpy(p + 12, &count[i], 4);
  }
  return bytes;
}

}  // namespace

FilmLayout FilmLayout::make(uint32_t width, uint32_t height, uint32_t tileSize) {
  FilmLayout l;
  l.width = width;
  l.height = height;
  l.tile_size = tileSize;
  l.tiles_x = (width + tileSize - 1) / tileSize;
  l.tiles_y = (height + tileSize - 1) / tileSize;
  return l;
}

TileRect FilmLayout::tileRect(uint32_t tile) const {
  const uint32_t tx = tile % tiles_x, ty = tile / tiles_x;
  return {tx * tile_size, ty * tile_size, std::min((tx + 1) * tile_size, width), std::min((ty + 1) * tile_size, height)};
}

uint32_t filmTileSize(uint32_t width, uint32_t height, uint32_t tileSize) {
  return uint64_t(width) * height < uint64_t(1920) * 1080 ? std::max(width, height) : tileSize;
}

uint64_t Film::fileBytes(const FilmLayout& l) {
  return kHeaderBytes + uint64_t(l.tileCount()) * kEntryBytes + uint64_t(l.width) * l.height * kPixelBytes;
}

std::unique_ptr<Film> Film::create(const std::filesystem::path& path, const FilmLayout& layout,
                                   const FilmIdentity& identity, bool keepInRam) {
  std::unique_ptr<Film> f(new Film());
  f->layout_ = layout;
  f->identity_ = identity;
  f->writable_ = true;
  f->file_ = BinaryFile(path, BinaryFile::Mode::Create);
  f->index_.assign(layout.tileCount(), {});
  f->dirty_.assign(layout.tileCount(), 0);
  f->index_offset_ = kHeaderBytes;
  f->data_offset_ = kHeaderBytes + uint64_t(layout.tileCount()) * kEntryBytes;
  f->data_end_ = f->data_offset_;
  if (keepInRam) {
    f->ram_mean_.assign(size_t(layout.width) * layout.height, Vec3f(0));
    f->ram_count_.assign(size_t(layout.width) * layout.height, 0);
  }
  f->checkpoint();  // a valid, empty film on disk from the start
  return f;
}

std::unique_ptr<Film> Film::open(const std::filesystem::path& path, bool writable, bool keepInRam) {
  std::unique_ptr<Film> f(new Film());
  f->writable_ = writable;
  f->file_ = BinaryFile(path, writable ? BinaryFile::Mode::ReadWrite : BinaryFile::Mode::Read);
  Header h{};
  if (f->file_.size() < kHeaderBytes) throw FileError(fmt::format("'{}' is not a Penumbra film", pathToUtf8(path)));
  f->file_.readAt(0, &h, sizeof(h));
  if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0)
    throw FileError(fmt::format("'{}' is not a Penumbra film", pathToUtf8(path)));
  if (h.version != kVersion)
    throw FileError(fmt::format("'{}' is film version {}, this Penumbra reads version {}", pathToUtf8(path), h.version,
                                kVersion));
  f->layout_ = FilmLayout::make(h.width, h.height, h.tile_size);
  if (f->layout_.tiles_x != h.tiles_x || f->layout_.tiles_y != h.tiles_y)
    throw FileError(fmt::format("'{}' has an inconsistent header", pathToUtf8(path)));
  f->identity_ = {h.settings_hash, h.scene_hash};
  f->index_offset_ = h.index_offset;
  f->data_offset_ = h.data_offset;
  f->data_end_ = h.data_end;
  if (f->data_end_ > f->file_.size())
    throw FileError(fmt::format("'{}' is shorter than its last checkpoint (truncated copy?)", pathToUtf8(path)));
  const uint32_t n = f->layout_.tileCount();
  f->index_.assign(n, {});
  f->dirty_.assign(n, 0);
  std::vector<uint8_t> raw(size_t(n) * kEntryBytes);
  if (!raw.empty()) f->file_.readAt(f->index_offset_, raw.data(), raw.size());
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* p = raw.data() + size_t(i) * kEntryBytes;
    Entry& e = f->index_[i];
    uint64_t checksum = 0;
    uint32_t flags = 0;
    std::memcpy(&e.slot[0], p, 8);
    std::memcpy(&e.slot[1], p + 8, 8);
    std::memcpy(&checksum, p + 16, 8);
    std::memcpy(&e.samples, p + 24, 4);
    std::memcpy(&flags, p + 28, 4);
    e.saved = e.live = (flags & kFlagSavedSlot1) ? 1 : 0;
    e.on_disk = e.saved_valid = (flags & kFlagPresent) != 0;
    e.checksum[e.saved] = checksum;
    e.written_samples = e.samples;
    if (!e.on_disk) e.samples = e.written_samples = 0;
  }
  if (writable) f->file_.truncate(f->data_end_);  // drop slots allocated after the last checkpoint
  if (keepInRam) {
    const FilmLayout& l = f->layout_;
    f->ram_mean_.assign(size_t(l.width) * l.height, Vec3f(0));
    f->ram_count_.assign(size_t(l.width) * l.height, 0);
    std::vector<Vec3f> m;
    std::vector<uint32_t> c;
    for (uint32_t t = 0; t < n; ++t) {
      if (!f->index_[t].on_disk) continue;
      f->readTileFromDisk(t, m, c);
      const TileRect r = l.tileRect(t);
      for (uint32_t y = r.y0; y < r.y1; ++y)
        for (uint32_t x = r.x0; x < r.x1; ++x) {
          const size_t src = size_t(y - r.y0) * r.width() + (x - r.x0), dst = size_t(y) * l.width + x;
          f->ram_mean_[dst] = m[src];
          f->ram_count_[dst] = c[src];
        }
    }
  }
  return f;
}

Film::~Film() { close(); }

void Film::close() { file_.close(); }

uint32_t Film::tilesWithData() const {
  uint32_t n = 0;
  for (const Entry& e : index_) n += e.samples > 0 ? 1 : 0;
  return n;
}

void Film::readTileFromDisk(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const {
  const TileRect r = layout_.tileRect(tile);
  mean.assign(r.pixels(), Vec3f(0));
  count.assign(r.pixels(), 0);
  const Entry& e = index_[tile];
  if (!e.on_disk) return;
  std::vector<uint8_t> bytes(r.pixels() * kPixelBytes);
  file_.readAt(e.slot[e.live], bytes.data(), bytes.size());
  if (hash64(bytes) != e.checksum[e.live])
    throw FileError(fmt::format("film '{}': tile {} is corrupt (checksum mismatch)", pathToUtf8(file_.path()), tile));
  for (size_t i = 0; i < mean.size(); ++i) {
    const uint8_t* p = bytes.data() + i * kPixelBytes;
    std::memcpy(&mean[i].x, p, 4);
    std::memcpy(&mean[i].y, p + 4, 4);
    std::memcpy(&mean[i].z, p + 8, 4);
    std::memcpy(&count[i], p + 12, 4);
  }
}

void Film::copyTileFromRam(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const {
  const TileRect r = layout_.tileRect(tile);
  mean.resize(r.pixels());
  count.resize(r.pixels());
  for (uint32_t y = r.y0; y < r.y1; ++y)
    for (uint32_t x = r.x0; x < r.x1; ++x) {
      const size_t dst = size_t(y - r.y0) * r.width() + (x - r.x0), src = size_t(y) * layout_.width + x;
      mean[dst] = ram_mean_[src];
      count[dst] = ram_count_[src];
    }
}

void Film::readTile(uint32_t tile, std::vector<Vec3f>& mean, std::vector<uint32_t>& count) const {
  if (inRam()) {
    copyTileFromRam(tile, mean, count);
  } else if (auto it = open_.find(tile); it != open_.end()) {
    mean = it->second.mean;
    count = it->second.count;
  } else {
    readTileFromDisk(tile, mean, count);
  }
}

void Film::readRect(const TileRect& rect, std::vector<Vec3f>& out, std::vector<uint32_t>* counts) const {
  const uint32_t w = rect.width();
  out.assign(rect.pixels(), Vec3f(0));
  if (counts) counts->assign(rect.pixels(), 0);
  if (inRam()) {
    for (uint32_t y = rect.y0; y < rect.y1; ++y)
      for (uint32_t x = rect.x0; x < rect.x1; ++x) {
        const size_t dst = size_t(y - rect.y0) * w + (x - rect.x0), src = size_t(y) * layout_.width + x;
        out[dst] = ram_mean_[src];
        if (counts) (*counts)[dst] = ram_count_[src];
      }
    return;
  }
  const uint32_t ts = layout_.tile_size;
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  for (uint32_t ty = rect.y0 / ts; ty * ts < rect.y1; ++ty) {
    for (uint32_t tx = rect.x0 / ts; tx * ts < rect.x1; ++tx) {
      const uint32_t tile = ty * layout_.tiles_x + tx;
      if (index_[tile].samples == 0) continue;
      readTile(tile, mean, count);
      const TileRect tr = layout_.tileRect(tile);
      for (uint32_t y = std::max(tr.y0, rect.y0); y < std::min(tr.y1, rect.y1); ++y)
        for (uint32_t x = std::max(tr.x0, rect.x0); x < std::min(tr.x1, rect.x1); ++x) {
          const size_t src = size_t(y - tr.y0) * tr.width() + (x - tr.x0), dst = size_t(y - rect.y0) * w + (x - rect.x0);
          out[dst] = mean[src];
          if (counts) (*counts)[dst] = count[src];
        }
    }
  }
}

void Film::writeTile(uint32_t tile, const std::vector<Vec3f>& mean, const std::vector<uint32_t>& count) {
  Entry& e = index_[tile];
  // Never overwrite the slot the saved index points at.
  const uint32_t target = e.saved_valid ? 1 - e.saved : e.saved;
  const std::vector<uint8_t> bytes = packBlock(mean, count);
  if (e.slot[target] == 0) {
    e.slot[target] = data_end_;
    data_end_ += bytes.size();
  }
  file_.writeAt(e.slot[target], bytes.data(), bytes.size());
  e.checksum[target] = hash64(bytes);
  e.live = target;
  e.written_samples = e.samples;
  e.on_disk = true;
}

void Film::merge(const TileResult& r, uint32_t tileSamples) {
  const uint32_t k = r.sample_end - r.sample_begin;
  if (k == 0) return;
  const uint32_t ts = layout_.tile_size;
  for (uint32_t ty = r.rect.y0 / ts; ty * ts < r.rect.y1; ++ty) {
    for (uint32_t tx = r.rect.x0 / ts; tx * ts < r.rect.x1; ++tx) {
      const uint32_t tile = ty * layout_.tiles_x + tx;
      const TileRect tr = layout_.tileRect(tile);
      Vec3f* mean = nullptr;
      uint32_t* count = nullptr;
      size_t stride = 0;
      Tile* cached = nullptr;
      if (inRam()) {
        mean = ram_mean_.data();
        count = ram_count_.data();
        stride = layout_.width;
      } else {
        auto it = open_.find(tile);
        if (it == open_.end()) {
          Tile t;
          readTileFromDisk(tile, t.mean, t.count);
          it = open_.emplace(tile, std::move(t)).first;
        }
        cached = &it->second;
        mean = cached->mean.data();
        count = cached->count.data();
        stride = tr.width();
      }
      for (uint32_t y = std::max(tr.y0, r.rect.y0); y < std::min(tr.y1, r.rect.y1); ++y) {
        for (uint32_t x = std::max(tr.x0, r.rect.x0); x < std::min(tr.x1, r.rect.x1); ++x) {
          const size_t i = cached ? size_t(y - tr.y0) * stride + (x - tr.x0) : size_t(y) * stride + x;
          const Vec3f& v = r.mean[size_t(y - r.rect.y0) * r.rect.width() + (x - r.rect.x0)];
          const uint32_t n = count[i];
          if (n == 0) {
            mean[i] = v;  // exact: a single result is stored bit for bit
          } else {
            const float w = static_cast<float>(k) / static_cast<float>(uint64_t(n) + k);
            mean[i] = mean[i] + (v - mean[i]) * w;
          }
          count[i] = n + k;
        }
      }
      index_[tile].samples = tileSamples;
      dirty_[tile] = 1;
    }
  }
}

void Film::flushTile(uint32_t tile) {
  if (inRam()) return;  // RAM films reach the disk at checkpoints
  auto it = open_.find(tile);
  if (it == open_.end()) return;
  if (dirty_[tile] && writable_) writeTile(tile, it->second.mean, it->second.count);
  dirty_[tile] = 0;
  open_.erase(it);
}

void Film::writeHeader() {
  Header h{};
  std::memcpy(h.magic, kMagic, sizeof(kMagic));
  h.version = kVersion;
  h.header_bytes = static_cast<uint32_t>(kHeaderBytes);
  h.width = layout_.width;
  h.height = layout_.height;
  h.tile_size = layout_.tile_size;
  h.tiles_x = layout_.tiles_x;
  h.tiles_y = layout_.tiles_y;
  h.index_offset = index_offset_;
  h.data_offset = data_offset_;
  h.data_end = data_end_;
  h.settings_hash = identity_.settings_hash;
  h.scene_hash = identity_.scene_hash;
  copyText(h.app_version, sizeof(h.app_version), appVersion());
  copyText(h.git_hash, sizeof(h.git_hash), gitHash());
  std::vector<uint8_t> bytes(kHeaderBytes, 0);
  std::memcpy(bytes.data(), &h, sizeof(h));
  file_.writeAt(0, bytes.data(), bytes.size());
}

void Film::checkpoint() {
  if (!writable_) return;
  // 1. Dirty tiles into their free slot.
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  for (uint32_t t = 0; t < dirty_.size(); ++t) {
    if (!dirty_[t]) continue;
    if (inRam()) {
      copyTileFromRam(t, mean, count);
      writeTile(t, mean, count);
    } else if (auto it = open_.find(t); it != open_.end()) {
      writeTile(t, it->second.mean, it->second.count);
    }
    dirty_[t] = 0;
  }
  file_.sync();  // tile data first, so the index never points at data that is not on disk
  // 2. The index now references the newest slot of every tile.
  std::vector<uint8_t> raw(index_.size() * kEntryBytes, 0);
  for (size_t i = 0; i < index_.size(); ++i) {
    Entry& e = index_[i];
    if (e.on_disk) e.saved = e.live;
    e.saved_valid = e.on_disk;
    uint8_t* p = raw.data() + i * kEntryBytes;
    const uint64_t checksum = e.checksum[e.saved];
    const uint32_t flags = (e.on_disk ? kFlagPresent : 0) | (e.saved ? kFlagSavedSlot1 : 0);
    std::memcpy(p, &e.slot[0], 8);
    std::memcpy(p + 8, &e.slot[1], 8);
    std::memcpy(p + 16, &checksum, 8);
    std::memcpy(p + 24, &e.written_samples, 4);
    std::memcpy(p + 28, &flags, 4);
  }
  if (!raw.empty()) file_.writeAt(index_offset_, raw.data(), raw.size());
  writeHeader();
  file_.sync();
}

}  // namespace pen
