#include "controller/scheduler.h"

#include <algorithm>
#include <cmath>

namespace pen {

Scheduler::Scheduler(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t sampleBegin, uint32_t sampleEnd,
                     const std::optional<PixelRect>& region) {
  TileRect area{0, 0, width, height};
  if (region) area = {region->x0, region->y0, region->x1, region->y1};
  const uint32_t ts = tileSize;
  const uint32_t tilesX = (width + ts - 1) / ts;

  // Tiles on the film's grid (so tile ids match the film), clipped to the region.
  for (uint32_t ty = 0; ty * ts < height; ++ty) {
    for (uint32_t tx = 0; tx < tilesX; ++tx) {
      TileRect r{tx * ts, ty * ts, std::min((tx + 1) * ts, width), std::min((ty + 1) * ts, height)};
      r = {std::max(r.x0, area.x0), std::max(r.y0, area.y0), std::min(r.x1, area.x1), std::min(r.y1, area.y1)};
      if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
      WorkItem item;
      item.id = items_.size();
      item.tile = ty * tilesX + tx;
      item.rect = r;
      item.sample_begin = sampleBegin;
      item.sample_end = sampleEnd;
      total_samples_ += r.pixels() * item.samples();
      items_.push_back(item);
    }
  }

  // Spiral from the centre: by ring (distance of the tile centre to the image centre), then by angle.
  order_.resize(items_.size());
  for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
  const double cx = width / 2.0, cy = height / 2.0;
  std::vector<std::pair<double, double>> key(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    const TileRect& r = items_[i].rect;
    const double dx = (r.x0 + r.x1) / 2.0 - cx, dy = (r.y0 + r.y1) / 2.0 - cy;
    key[i] = {std::round(std::max(std::fabs(dx), std::fabs(dy)) / ts), std::atan2(dy, dx)};
  }
  std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
  done_.assign(items_.size(), false);
}

uint64_t Scheduler::skipDone(const std::function<bool(uint32_t tile)>& tileDone) {
  uint64_t samples = 0;
  for (WorkItem& item : items_) {
    if (done_[item.id] || !tileDone(item.tile)) continue;
    done_[item.id] = true;
    ++completed_;
    samples += item.rect.pixels() * item.samples();
  }
  return samples;
}

bool Scheduler::hasPending() const {
  if (!requeued_.empty()) return true;
  for (size_t i = cursor_; i < order_.size(); ++i)
    if (!done_[order_[i]]) return true;
  return false;
}

std::optional<WorkItem> Scheduler::next() {
  while (true) {
    uint32_t index;
    if (!requeued_.empty()) {
      index = requeued_.front();
      requeued_.erase(requeued_.begin());
    } else if (cursor_ < order_.size()) {
      index = order_[cursor_++];
    } else {
      return std::nullopt;
    }
    if (done_[index]) continue;  // skipped on resume
    outstanding_.insert(items_[index].id);
    return items_[index];
  }
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
