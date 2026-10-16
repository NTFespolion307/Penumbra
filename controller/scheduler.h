#pragma once

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "core/render/work.h"
#include "core/settings/render_settings.h"

namespace pen {

// Splits the frame into work items and hands them out (docs/GUIDE.md "Scheduling, leases and tiles").
// M1: one item per tile covering the whole sample range, tiles in a spiral from the image centre; frames
// below 1080p use a single full-frame tile. Leases with timeouts, requeue and sample-range splitting: M3.
class Scheduler {
 public:
  Scheduler(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t sampleBegin, uint32_t sampleEnd,
            const std::optional<PixelRect>& region);

  std::optional<WorkItem> next();
  // Marks an item merged; returns false for an unknown or already completed item (duplicate).
  bool complete(uint64_t id);
  void requeue(uint64_t id);

  bool done() const { return completed_ == items_.size(); }
  uint32_t total() const { return static_cast<uint32_t>(items_.size()); }
  uint32_t completed() const { return completed_; }
  uint32_t outstanding() const { return static_cast<uint32_t>(outstanding_.size()); }
  bool hasPending() const { return cursor_ < order_.size() || !requeued_.empty(); }
  uint64_t totalSamples() const { return total_samples_; }
  const std::vector<WorkItem>& items() const { return items_; }

 private:
  std::vector<WorkItem> items_;
  std::vector<uint32_t> order_;  // issue order (spiral)
  size_t cursor_ = 0;
  std::vector<uint32_t> requeued_;
  std::unordered_set<uint64_t> outstanding_;
  std::vector<bool> done_;
  uint32_t completed_ = 0;
  uint64_t total_samples_ = 0;
};

}  // namespace pen
