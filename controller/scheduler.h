#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "core/film/film.h"
#include "core/render/work.h"
#include "core/settings/render_settings.h"

namespace pen {

// RAM films refine the whole image pass by pass (breadth-first); disk films finish one tile after the other
// (depth-first) so few tiles are in memory at once (docs/GUIDE.md "Pass order").
enum class PassOrder { BreadthFirst, DepthFirst };

inline constexpr uint32_t kUnboundedSamples = std::numeric_limits<uint32_t>::max();

// Samples per chunk for a tile: the largest power of two with tilePixels x chunk <= 2^22 (a few seconds on a
// CPU), at least 1 and at most the range. Depends only on the settings, never on the device (D-045).
uint32_t chunkSamples(uint64_t tilePixels, uint32_t rangeSamples);

// Splits the frame into work items: film tiles (clipped to --region), spiral from the image centre, each cut
// into fixed sample chunks. The controller tells it which chunks are merged; items lost to a failed or slow
// device are requeued and handed out first.
class Scheduler {
 public:
  // sampleEnd = kUnboundedSamples renders until stopNew() (--spp 0 with --time-limit).
  Scheduler(const FilmLayout& layout, uint32_t sampleBegin, uint32_t sampleEnd,
            const std::optional<PixelRect>& region, PassOrder order);

  // Resume: `samplesInFilm(tile)` samples (whole chunks) of each tile are already in the film.
  void restore(const std::function<uint32_t(uint32_t tile)>& samplesInFilm);

  std::optional<WorkItem> next();
  void requeue(const WorkItem& item);
  void stopNew() { stopped_ = true; }
  bool stopped() const { return stopped_; }

  // Merge bookkeeping, in chunk order per tile.
  uint32_t mergedChunks(uint32_t tile) const;
  void markMerged(uint32_t tile);
  bool tileComplete(uint32_t tile) const;
  bool knowsTile(uint32_t tile) const { return tile < lookup_.size() && lookup_[tile] >= 0; }
  // The item for chunk `chunk` of film tile `tile` (validation of results).
  WorkItem item(uint32_t tile, uint32_t chunk) const;
  uint32_t chunkCount(uint32_t tile) const;

  bool done() const;  // every chunk of every tile merged (never true for an unbounded render)
  bool unbounded() const { return sample_end_ == kUnboundedSamples; }
  uint32_t tileCount() const { return static_cast<uint32_t>(tiles_.size()); }
  uint32_t tilesComplete() const;
  uint64_t totalSamples() const { return total_samples_; }  // 0 when unbounded
  uint64_t mergedSamples() const { return merged_samples_; }
  PassOrder order() const { return order_; }

 private:
  struct TileWork {
    uint32_t tile = 0;
    TileRect rect;
    uint32_t chunk_size = 1;
    uint32_t chunks = 0;  // kUnboundedSamples when unbounded
    uint32_t issued = 0;  // chunks handed out (requeues not counted)
    uint32_t merged = 0;
  };
  WorkItem makeItem(const TileWork& t, uint32_t chunk) const;

  std::vector<TileWork> tiles_;  // spiral order
  std::vector<int32_t> lookup_;  // film tile -> index in tiles_, -1 if outside the region
  uint32_t sample_begin_, sample_end_;
  PassOrder order_;
  std::deque<WorkItem> requeued_;
  size_t cursor_ = 0;
  uint32_t pass_ = 0;
  bool stopped_ = false;
  uint64_t total_samples_ = 0, merged_samples_ = 0;
};

}  // namespace pen
