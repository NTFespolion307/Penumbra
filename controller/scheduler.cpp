#include "controller/scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
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
                     const std::optional<PixelRect>& region, PassOrder order, const std::vector<AovPass>& aovs)
    : sample_begin_(sampleBegin), sample_end_(sampleEnd), order_(order) {
  TileRect area{0, 0, layout.width, layout.height};
  if (region) area = {region->x0, region->y0, region->x1, region->y1};
  tile_count_ = layout.tileCount();
  if (tile_count_ >= (1u << 24)) throw std::runtime_error("more than 2^24 film tiles");  // item ids hold 24 bits
  lookup_.assign(size_t(kPassCount) * tile_count_, -1);

  // Spiral from the centre: by ring (distance of the tile centre to the image centre), then by angle.
  const double cx = layout.width / 2.0, cy = layout.height / 2.0, ts = layout.tile_size;
  const auto key = [&](const TileWork& t) {
    const double dx = (t.rect.x0 + t.rect.x1) / 2.0 - cx, dy = (t.rect.y0 + t.rect.y1) / 2.0 - cy;
    return std::make_pair(std::round(std::max(std::fabs(dx), std::fabs(dy)) / ts), std::atan2(dy, dx));
  };
  const auto addPass = [&](Pass pass, uint32_t begin, uint32_t end) {
    const uint32_t range = end == kUnboundedSamples ? kUnboundedSamples : end - begin;
    const size_t first = tiles_.size();
    for (uint32_t tile = 0; tile < tile_count_; ++tile) {
      TileRect r = layout.tileRect(tile);
      r = {std::max(r.x0, area.x0), std::max(r.y0, area.y0), std::min(r.x1, area.x1), std::min(r.y1, area.y1)};
      if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
      TileWork t;
      t.tile = tile;
      t.pass = pass;
      t.begin = begin;
      t.end = end;
      t.rect = r;
      t.chunk_size = chunkSamples(r.pixels(), range);
      t.chunks = range == kUnboundedSamples ? kUnboundedSamples : (range + t.chunk_size - 1) / t.chunk_size;
      if (pass == Pass::Beauty && range != kUnboundedSamples) total_samples_ += r.pixels() * range;
      tiles_.push_back(t);
    }
    std::stable_sort(tiles_.begin() + std::ptrdiff_t(first), tiles_.end(),
                     [&](const TileWork& a, const TileWork& b) { return key(a) < key(b); });
  };
  for (const AovPass& a : aovs) addPass(a.pass, 0, a.samples);  // cheap and needed by the denoiser: first
  addPass(Pass::Beauty, sampleBegin, sampleEnd);
  for (size_t i = 0; i < tiles_.size(); ++i)
    lookup_[size_t(tiles_[i].pass) * tile_count_ + tiles_[i].tile] = static_cast<int32_t>(i);
}

std::vector<AovPass> aovPassesFor(const RenderSettings& s) {
  std::vector<AovPass> v;
  for (const Pass p : {Pass::Albedo, Pass::Normal, Pass::Depth}) {
    const bool listed = std::find(s.aov.begin(), s.aov.end(), passName(p)) != s.aov.end();
    const bool denoiser = s.denoise != Denoiser::None && (p == Pass::Albedo || p == Pass::Normal);
    if (listed || denoiser) v.push_back({p, kAovSamples});
  }
  return v;
}

bool Scheduler::hasPass(Pass pass) const {
  for (const TileWork& t : tiles_)
    if (t.pass == pass) return true;
  return false;
}

void Scheduler::restore(const std::function<uint32_t(uint32_t tile)>& samplesInFilm, Pass pass) {
  uint32_t minMerged = kUnboundedSamples;
  for (TileWork& t : tiles_) {
    if (t.pass != pass) continue;
    const uint32_t s = samplesInFilm(t.tile);
    // Chunks are fixed, so the film always holds whole chunks (or the whole range).
    uint32_t chunks = s / t.chunk_size;
    if (t.chunks != kUnboundedSamples) chunks = std::min(chunks, t.chunks);
    if (s >= t.end - t.begin && t.chunks != kUnboundedSamples) chunks = t.chunks;
    t.merged = t.issued = chunks;
    if (pass == Pass::Beauty) merged_samples_ += t.rect.pixels() * std::min<uint64_t>(s, uint64_t(chunks) * t.chunk_size);
    minMerged = std::min(minMerged, chunks);
  }
  if (pass == Pass::Beauty) pass_ = minMerged == kUnboundedSamples ? 0 : minMerged;
  cursor_ = 0;
}

WorkItem Scheduler::makeItem(const TileWork& t, uint32_t chunk, uint32_t span) const {
  WorkItem item;
  item.tile = t.tile;
  item.chunk = chunk;
  item.pass = t.pass;
  item.id = WorkItem::makeId(t.tile, chunk, t.pass);
  item.rect = t.rect;
  item.sample_begin = t.begin + chunk * t.chunk_size;
  const uint64_t end = uint64_t(item.sample_begin) + uint64_t(t.chunk_size) * span;
  item.sample_end = static_cast<uint32_t>(std::min<uint64_t>(end, t.end));
  return item;
}

WorkItem Scheduler::item(uint32_t tile, uint32_t chunk, Pass pass) const {
  return makeItem(tiles_[index(tile, pass)], chunk);
}

uint32_t Scheduler::chunkCount(uint32_t tile, Pass pass) const { return tiles_[index(tile, pass)].chunks; }

std::optional<WorkItem> Scheduler::next(uint32_t span) {
  span = std::max(span, 1u);
  // Up to `span` chunks of tile t from its next unissued one.
  const auto take = [&](TileWork& t) {
    const uint32_t n = t.chunks == kUnboundedSamples ? span : std::min(span, t.chunks - t.issued);
    WorkItem item = makeItem(t, t.issued, n);
    t.issued += n;
    return item;
  };
  if (!requeued_.empty()) {  // lost or late items first, even after stopNew (their tiles wait for them)
    WorkItem item = requeued_.front();
    requeued_.pop_front();
    return item;
  }
  if (stopped_) {  // the AOVs still get done (the denoiser needs them); they are cheap
    for (TileWork& t : tiles_)
      if (t.pass != Pass::Beauty && t.issued < t.chunks) return take(t);
    return std::nullopt;
  }
  if (tiles_.empty()) return std::nullopt;

  if (order_ == PassOrder::DepthFirst) {
    while (cursor_ < tiles_.size()) {
      TileWork& t = tiles_[cursor_];
      if (t.issued < t.chunks) return take(t);
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
      if (t.issued <= pass_ && t.issued < t.chunks) return take(t);
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

uint32_t Scheduler::mergedChunks(uint32_t tile, Pass pass) const { return tiles_[index(tile, pass)].merged; }

void Scheduler::markMerged(uint32_t tile, Pass pass) {
  TileWork& t = tiles_[index(tile, pass)];
  const WorkItem item = makeItem(t, t.merged);
  if (pass == Pass::Beauty) merged_samples_ += t.rect.pixels() * item.samples();
  ++t.merged;
}

void Scheduler::markMergedUntil(uint32_t tile, uint32_t sampleEnd, Pass pass) {
  TileWork& t = tiles_[index(tile, pass)];
  do {
    if (pass == Pass::Beauty) merged_samples_ += t.rect.pixels() * makeItem(t, t.merged).samples();
    ++t.merged;
  } while (t.merged < t.chunks && makeItem(t, t.merged).sample_begin < sampleEnd);
}

uint32_t Scheduler::chunksIn(const WorkItem& item) const {
  if (!knowsTile(item.tile, item.pass)) return 1;
  const uint32_t size = tiles_[index(item.tile, item.pass)].chunk_size;
  return std::max(1u, (item.samples() + size - 1) / size);
}

bool Scheduler::tileComplete(uint32_t tile, Pass pass) const {
  const TileWork& t = tiles_[index(tile, pass)];
  return t.chunks != kUnboundedSamples && t.merged >= t.chunks;
}

uint32_t Scheduler::tilesComplete() const {
  uint32_t n = 0;
  for (const TileWork& t : tiles_)
    n += (t.pass == Pass::Beauty && t.chunks != kUnboundedSamples && t.merged >= t.chunks) ? 1 : 0;
  return n;
}

uint32_t Scheduler::tileCount() const {
  uint32_t n = 0;
  for (const TileWork& t : tiles_) n += t.pass == Pass::Beauty ? 1 : 0;
  return n;
}

bool Scheduler::aovsDone() const {
  for (const TileWork& t : tiles_)
    if (t.pass != Pass::Beauty && t.merged < t.chunks) return false;
  return true;
}

bool Scheduler::done() const {
  if (unbounded()) return false;
  for (const TileWork& t : tiles_)
    if (t.merged < t.chunks) return false;
  return true;
}

}  // namespace pen
