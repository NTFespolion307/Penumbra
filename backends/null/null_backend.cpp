#include "backends/null/null_backend.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include <thread>

#include "core/sampling/sampler.h"

namespace pen {

namespace {

class NullBackend final : public IBackend {
 public:
  explicit NullBackend(uint32_t index) {
    info_.name = fmt::format("null test device {}", index);
    info_.backend = BackendKind::Null;
    info_.threads = 1;
    // PENUMBRA_TEST_NULL_DELAY_MS (test-only, D-070): each item takes this long, like a real device would.
    if (const char* d = std::getenv("PENUMBRA_TEST_NULL_DELAY_MS")) delay_ms_ = std::atoi(d);
  }
  DeviceInfo info() const override { return info_; }
  MemoryPlan plan(const SceneStats&) override { return {0, "tier 0, test backend (no scene upload)"}; }
  void upload(const Scene&, const MemoryPlan&) override {}
  void prepare(const FrameSetup&) override { cancel_ = false; }
  void cancel() override { cancel_ = true; }

  void render(const WorkItem& item, TileResult& out) override {
    out.item_id = item.id;
    out.rect = item.rect;
    out.sample_begin = item.sample_begin;
    out.sample_end = item.sample_end;
    out.mean.resize(item.rect.pixels());
    size_t i = 0;
    for (uint32_t y = item.rect.y0; y < item.rect.y1; ++y)
      for (uint32_t x = item.rect.x0; x < item.rect.x1; ++x) {
        const uint32_t h = hashCombine(hashCombine(hashU32(x), y), item.sample_begin * 7919u + item.sample_end);
        out.mean[i++] = {toUnitFloat(h), toUnitFloat(hashU32(h)), toUnitFloat(hashU32(h ^ 0x9e3779b9u))};
      }
    for (int waited = 0; waited < delay_ms_ && !cancel_.load(); waited += 5)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    out.cancelled = cancel_.load();
  }

 private:
  DeviceInfo info_;
  std::atomic<bool> cancel_{false};
  int delay_ms_ = 0;
};

}  // namespace

std::unique_ptr<IBackend> createNullBackend(uint32_t index) { return std::make_unique<NullBackend>(index); }

}  // namespace pen
