#include "controller/scheduler.h"

#include <algorithm>
#include <cmath>

namespace pen {

Scheduler::Scheduler(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t sampleBegin, uint32_t sampleEnd,
                     const std::optional<PixelRect>& region) {
  TileRect area{0, 0, width, height};
  if (region) area = {region->x0, region->y0, region->x1, region->y1};
  const bool singleTile = uint64_t(width) * height < uint64_t(1920) * 1080;
  const uint32_t ts = singleTile ? std::max(area.width(), area.height()) : tileSize;

  // Tiles on the full-image grid (so tile ids do not depend on the region), clipped to the region.
  for (uint32_t ty = 0; ty * ts < height; ++ty) {
    for (uint32_t tx = 0; tx * ts < width; ++tx) {
      TileRect r{tx * ts, ty * ts, std::min((tx + 1) * ts, width), std::min((ty + 1) * ts, height)};
      if (singleTile) r = area;
      r = {std::max(r.x0, area.x0), std::max(r.y0, area.y0), std::min(r.x1, area.x1), std::min(r.y1, area.y1)};
      if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
      WorkItem item;
      item.id = items_.size();
      item.tile = ty * ((width + ts - 1) / ts) + tx;
      item.rect = r;
      item.sample_begin = sampleBegin;
      item.sample_end = sampleEnd;
      total_samples_ += r.pixels() * item.samples();
      items_.push_back(item);
      if (singleTile) break;
    }
    if (singleTile) break;
  }

  // Spiral from the centre: by distance of the tile centre to the image centre, then by angle.
  order_.resize(items_.size());
  for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
  const double cx = width / 2.0, cy = height / 2.0;
  std::vector<std::pair<double, double>> key(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    const TileRect& r = items_[i].rect;
    const double dx = (r.x0 + r.x1) / 2.0 - cx, dy = (r.y0 + r.y1) / 2.0 - cy;
    key[i] = {std::round(std::max(std::fabs(dx), std::fabs(dy)) / tileSize), std::atan2(dy, dx)};
  }
  std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
  done_.assign(items_.size(), false);
}

std::optional<WorkItem> Scheduler::next() {
  uint32_t index;
  if (!requeued_.empty()) {
    index = requeued_.front();
    requeued_.erase(requeued_.begin());
  } else if (cursor_ < order_.size()) {
    index = order_[cursor_++];
  } else {
    return std::nullopt;
  }
  outstanding_.insert(items_[index].id);
  return items_[index];
}

bool Scheduler::complete(uint64_t id) {
  if (id >= items_.size() || done_[id]) return false;
  done_[id] = true;
  outstanding_.erase(id);
  ++completed_;
  return true;
}

void Scheduler::requeue(uint64_t id) {
  if (id >= items_.size() || done_[id] || !outstanding_.erase(id)) return;
  requeued_.push_back(static_cast<uint32_t>(id));
}

}  // namespace pen
