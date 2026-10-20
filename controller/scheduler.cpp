#include "controller/scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <spdlog/spdlog.h>

namespace pen {

uint64_t chunkBudget() {
  static const uint64_t budget = [] {
    if (const char* v = std::getenv("PENUMBRA_TEST_CHUNK_BUDGET")) {
      const uint64_t b = std::strtoull(v, nullptr, 10);
      if (b > 0) {
        spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_CHUNK_BUDGET): work items of {} pixel-samples", b);
        return b;
      }
    }
    return uint64_t(1) << 26;
  }();
  return budget;
}

uint32_t chunkSamples(uint64_t tilePixels, uint32_t rangeSamples, uint64_t budget) {
  uint64_t c = tilePixels > 0 ? budget / tilePixels : budget;
  uint32_t p = 1;
  while (uint64_t(p) * 2 <= c && p < (1u << 30)) p *= 2;
  return std::max(1u, std::min(p, rangeSamples));
}

Scheduler::Scheduler(const FilmLayout& layout, uint32_t sampleBegin, uint32_t sampleEnd,
                     const std::optional<PixelRect>& region, PassOrder order)
    : sample_begin_(sampleBegin), sample_end_(sampleEnd), order_(order) {
  TileRect area{0, 0, layout.width, layout.height};
  if (region) area = {region->x0, region->y0, region->x1, region->y1};
  const uint32_t range = sampleEnd == kUnboundedSamples ? kUnboundedSamples : sampleEnd - sampleBegin;
  lookup_.assign(layout.tileCount(), -1);

  for (uint32_t tile = 0; tile < layout.tileCount(); ++tile) {
    TileRect r = layout.tileRect(tile);
    r = {std::max(r.x0, area.x0), std::max(r.y0, area.y0), std::min(r.x1, area.x1), std::min(r.y1, area.y1)};
    if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
    TileWork t;
    t.tile = tile;
    t.rect = r;
    t.chunk_size = chunkSamples(r.pixels(), range);
    t.chunks = range == kUnboundedSamples ? kUnboundedSamples : (range + t.chunk_size - 1) / t.chunk_size;
    if (range != kUnboundedSamples) total_samples_ += r.pixels() * range;
    tiles_.push_back(t);
  }

  // Spiral from the centre: by ring (distance of the tile centre to the image centre), then by angle.
  const double cx = layout.width / 2.0, cy = layout.height / 2.0, ts = layout.tile_size;
  const auto key = [&](const TileWork& t) {
    const double dx = (t.rect.x0 + t.rect.x1) / 2.0 - cx, dy = (t.rect.y0 + t.rect.y1) / 2.0 - cy;
    return std::make_pair(std::round(std::max(std::fabs(dx), std::fabs(dy)) / ts), std::atan2(dy, dx));
  };
  std::stable_sort(tiles_.begin(), tiles_.end(), [&](const TileWork& a, const TileWork& b) { return key(a) < key(b); });
  for (size_t i = 0; i < tiles_.size(); ++i) lookup_[tiles_[i].tile] = static_cast<int32_t>(i);
}

void Scheduler::restore(const std::function<uint32_t(uint32_t tile)>& samplesInFilm) {
  uint32_t minMerged = kUnboundedSamples;
  for (TileWork& t : tiles_) {
    const uint32_t s = samplesInFilm(t.tile);
    // Chunks are fixed, so the film always holds whole chunks (or the whole range).
    uint32_t chunks = s / t.chunk_size;
    if (t.chunks != kUnboundedSamples) chunks = std::min(chunks, t.chunks);
    if (s >= sample_end_ - sample_begin_ && t.chunks != kUnboundedSamples) chunks = t.chunks;
    t.merged = t.issued = chunks;
    merged_samples_ += t.rect.pixels() * std::min<uint64_t>(s, uint64_t(chunks) * t.chunk_size);
    minMerged = std::min(minMerged, chunks);
  }
  pass_ = minMerged == kUnboundedSamples ? 0 : minMerged;
  cursor_ = 0;
}

WorkItem Scheduler::makeItem(const TileWork& t, uint32_t chunk) const {
  WorkItem item;
  item.tile = t.tile;
  item.chunk = chunk;
  item.id = WorkItem::makeId(t.tile, chunk);
  item.rect = t.rect;
  item.sample_begin = sample_begin_ + chunk * t.chunk_size;
  const uint64_t end = uint64_t(item.sample_begin) + t.chunk_size;
  item.sample_end = static_cast<uint32_t>(std::min<uint64_t>(end, sample_end_));
  return item;
}

WorkItem Scheduler::item(uint32_t tile, uint32_t chunk) const { return makeItem(tiles_[lookup_[tile]], chunk); }

uint32_t Scheduler::chunkCount(uint32_t tile) const { return tiles_[lookup_[tile]].chunks; }

std::optional<WorkItem> Scheduler::next() {
  if (!requeued_.empty()) {  // lost or late items first, even after stopNew (their tiles wait for them)
    WorkItem item = requeued_.front();
    requeued_.pop_front();
    return item;
  }
  if (stopped_ || tiles_.empty()) return std::nullopt;

  if (order_ == PassOrder::DepthFirst) {
    while (cursor_ < tiles_.size()) {
      TileWork& t = tiles_[cursor_];
      if (t.issued < t.chunks) return makeItem(t, t.issued++);
      ++cursor_;
    }
    return std::nullopt;
  }

  // Breadth-first: in pass p every tile with fewer than p + 1 chunks gets one more.
  for (;;) {
    bool anyLeft = false;
    while (cursor_ < tiles_.size()) {
      TileWork& t = tiles_[cursor_++];
      if (t.issued < t.chunks) anyLeft = true;
      if (t.issued <= pass_ && t.issued < t.chunks) return makeItem(t, t.issued++);
    }
    // End of a pass: start the next one, unless no tile has chunks left.
    if (!anyLeft) {
      bool more = false;
      for (const TileWork& t : tiles_) more = more || t.issued < t.chunks;
      if (!more) return std::nullopt;
    }
    ++pass_;
    cursor_ = 0;
  }
}

void Scheduler::requeue(const WorkItem& item) {
  for (const WorkItem& q : requeued_)
    if (q.id == item.id) return;
  WorkItem copy = item;
  copy.lease = 0;
  requeued_.push_back(copy);
}

uint32_t Scheduler::mergedChunks(uint32_t tile) const { return tiles_[lookup_[tile]].merged; }

void Scheduler::markMerged(uint32_t tile) {
  TileWork& t = tiles_[lookup_[tile]];
  const WorkItem item = makeItem(t, t.merged);
  merged_samples_ += t.rect.pixels() * item.samples();
  ++t.merged;
}

bool Scheduler::tileComplete(uint32_t tile) const {
  const TileWork& t = tiles_[lookup_[tile]];
  return t.chunks != kUnboundedSamples && t.merged >= t.chunks;
}

uint32_t Scheduler::tilesComplete() const {
  uint32_t n = 0;
  for (const TileWork& t : tiles_) n += (t.chunks != kUnboundedSamples && t.merged >= t.chunks) ? 1 : 0;
  return n;
}

bool Scheduler::done() const {
  if (unbounded()) return false;
  for (const TileWork& t : tiles_)
    if (t.merged < t.chunks) return false;
  return true;
}

}  // namespace pen
