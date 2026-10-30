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

// Pixel-samples per work item: 2^26 (about 0.3 s on a mid-range RTX GPU, a few seconds on a many-core CPU;
// DECISIONS D-083). Tests that need many items set PENUMBRA_TEST_CHUNK_BUDGET (test only, logged when active).
uint64_t chunkBudget();

// Samples per chunk for a tile: the largest power of two with tilePixels x chunk <= budget, at least 1 and at
// most the range. Depends only on the settings, never on the device (D-045).
uint32_t chunkSamples(uint64_t tilePixels, uint32_t rangeSamples, uint64_t budget = chunkBudget());

// Splits the frame into work items: film tiles (clipped to --region), spiral from the image centre, each cut
// into fixed sample chunks. The controller tells it which chunks are merged; items lost to a failed or slow
// device are requeued and handed out first.
// An AOV pass to render besides the image (M11, D-127): its samples per pixel, from sample 0.
struct AovPass {
  Pass pass;
  uint32_t samples;
};

// The AOV passes a render needs: --aov, plus albedo and normal for the denoiser. Each takes kAovSamples per pixel
// (enough for clean, antialiased first-hit values; a few percent of a usual render).
inline constexpr uint32_t kAovSamples = 16;
std::vector<AovPass> aovPassesFor(const RenderSettings& s);

class Scheduler {
 public:
  // sampleEnd = kUnboundedSamples renders until stopNew() (--spp 0 with --time-limit). AOV tiles come first, are
  // handed out even after stopNew (the denoiser needs them) and do not count in the totals and progress below.
  Scheduler(const FilmLayout& layout, uint32_t sampleBegin, uint32_t sampleEnd,
            const std::optional<PixelRect>& region, PassOrder order, const std::vector<AovPass>& aovs = {});

  // Resume: `samplesInFilm(tile)` samples (whole chunks) of each tile of `pass` are already in its film.
  void restore(const std::function<uint32_t(uint32_t tile)>& samplesInFilm, Pass pass = Pass::Beauty);

  // The next item: `span` consecutive chunks of one tile when that many are left (an upload-bound worker sends
  // one result for them, D-116), else one chunk. Requeued items keep the span they were issued with.
  std::optional<WorkItem> next(uint32_t span = 1);
  void requeue(const WorkItem& item);
  void stopNew() { stopped_ = true; }
  bool stopped() const { return stopped_; }

  // Merge bookkeeping, in chunk order per tile and pass.
  uint32_t mergedChunks(uint32_t tile, Pass pass = Pass::Beauty) const;
  void markMerged(uint32_t tile, Pass pass = Pass::Beauty);
  // Every chunk of `tile` that ends at or before `sampleEnd` (a merged item of one or more chunks).
  void markMergedUntil(uint32_t tile, uint32_t sampleEnd, Pass pass = Pass::Beauty);
  // How many chunks an item covers (1, or its span).
  uint32_t chunksIn(const WorkItem& item) const;
  bool tileComplete(uint32_t tile, Pass pass = Pass::Beauty) const;
  bool knowsTile(uint32_t tile, Pass pass = Pass::Beauty) const { return index(tile, pass) >= 0; }
  // The item for chunk `chunk` of film tile `tile` (validation of results).
  WorkItem item(uint32_t tile, uint32_t chunk, Pass pass = Pass::Beauty) const;
  uint32_t chunkCount(uint32_t tile, Pass pass = Pass::Beauty) const;
  bool hasPass(Pass pass) const;

  bool done() const;  // every chunk of every tile and pass merged (never true for an unbounded render)
  bool aovsDone() const;  // every AOV tile merged (true without AOVs)
  bool unbounded() const { return sample_end_ == kUnboundedSamples; }
  uint32_t tileCount() const;  // the image's tiles
  uint32_t tilesComplete() const;
  uint64_t totalSamples() const { return total_samples_; }  // 0 when unbounded
  uint64_t mergedSamples() const { return merged_samples_; }
  PassOrder order() const { return order_; }

 private:
  struct TileWork {
    uint32_t tile = 0;
    Pass pass = Pass::Beauty;
    uint32_t begin = 0, end = 0;  // its pass's sample range (end kUnboundedSamples when unbounded)
    TileRect rect;
    uint32_t chunk_size = 1;
    uint32_t chunks = 0;  // kUnboundedSamples when unbounded
    uint32_t issued = 0;  // chunks handed out (requeues not counted)
    uint32_t merged = 0;
  };
  WorkItem makeItem(const TileWork& t, uint32_t chunk, uint32_t span = 1) const;

  int32_t index(uint32_t tile, Pass pass) const {
    const size_t k = size_t(pass) * tile_count_ + tile;
    return tile < tile_count_ && k < lookup_.size() ? lookup_[k] : -1;
  }
  std::vector<TileWork> tiles_;  // AOV passes first, then the image; each in spiral order
  uint32_t tile_count_ = 0;      // film tiles
  std::vector<int32_t> lookup_;  // pass x film tile -> index in tiles_, -1 if outside the region or no such pass
  uint32_t sample_begin_, sample_end_;
  PassOrder order_;
  std::deque<WorkItem> requeued_;
  size_t cursor_ = 0;
  uint32_t pass_ = 0;
  bool stopped_ = false;
  uint64_t total_samples_ = 0, merged_samples_ = 0;
};

}  // namespace pen
