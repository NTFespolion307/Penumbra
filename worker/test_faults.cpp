#include "worker/test_faults.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "core/settings/parse.h"

namespace pen {

namespace {

std::atomic<bool> g_frozen{false};
std::atomic<uint32_t> g_corrupt_tiles{0};

class FaultyBackend final : public IBackend {
 public:
  FaultyBackend(std::unique_ptr<IBackend> inner, const FaultSpec& spec) : inner_(std::move(inner)), spec_(spec) {}
  DeviceInfo info() const override { return inner_->info(); }
  MemoryPlan plan(const SceneStats& s) override { return inner_->plan(s); }
  void upload(const Scene& scene, const MemoryPlan& plan) override {
    if (spec_.mode == FaultMode::Upload) throw BackendError("injected test fault while taking the scene (PENUMBRA_TEST_FAULTS)");
    inner_->upload(scene, plan);
  }
  void prepare(const FrameSetup& frame) override {
    cancel_ = false;
    inner_->prepare(frame);
  }
  void cancel() override {
    cancel_ = true;
    inner_->cancel();
  }
  void render(const WorkItem& item, TileResult& out) override {
    if (rendered_++ >= spec_.after) {
      switch (spec_.mode) {
        case FaultMode::Throw: throw BackendError("injected test fault (PENUMBRA_TEST_FAULTS)");
        case FaultMode::Exit: std::_Exit(137);
        case FaultMode::Corrupt:
          inner_->render(item, out);
          ++g_corrupt_tiles;
          return;
        case FaultMode::Upload: break;
        case FaultMode::Freeze: g_frozen = true; [[fallthrough]];
        case FaultMode::Hang:
          while (!cancel_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
          out.item_id = item.id;
          out.cancelled = true;
          return;
      }
    }
    inner_->render(item, out);
  }

 private:
  std::unique_ptr<IBackend> inner_;
  FaultSpec spec_;
  uint32_t rendered_ = 0;
  std::atomic<bool> cancel_{false};
};

}  // namespace

std::optional<FaultSpec> faultSpecFromEnvironment() {
  const char* env = std::getenv("PENUMBRA_TEST_FAULTS");
  if (!env || !*env) return std::nullopt;
  FaultSpec spec;
  std::string_view s = env;
  while (!s.empty()) {
    const size_t comma = s.find(',');
    const std::string_view item = s.substr(0, comma);
    const size_t eq = item.find('=');
    if (eq == std::string_view::npos) throw std::invalid_argument("PENUMBRA_TEST_FAULTS: expected key=value");
    const std::string_view key = item.substr(0, eq), value = item.substr(eq + 1);
    if (key == "device") spec.device = static_cast<uint32_t>(parseUint(value, 0, 255));
    else if (key == "after") spec.after = static_cast<uint32_t>(parseUint(value, 0, 1u << 30));
    else if (key == "mode" && value == "throw") spec.mode = FaultMode::Throw;
    else if (key == "mode" && value == "hang") spec.mode = FaultMode::Hang;
    else if (key == "mode" && value == "exit") spec.mode = FaultMode::Exit;
    else if (key == "mode" && value == "freeze") spec.mode = FaultMode::Freeze;
    else if (key == "mode" && value == "corrupt") spec.mode = FaultMode::Corrupt;
    else if (key == "mode" && value == "upload") spec.mode = FaultMode::Upload;
    else throw std::invalid_argument("PENUMBRA_TEST_FAULTS: use device=N,after=K,mode=throw|hang|exit|freeze|corrupt|upload");
    if (comma == std::string_view::npos) break;
    s.remove_prefix(comma + 1);
  }
  return spec;
}

std::string_view toString(FaultMode mode) {
  switch (mode) {
    case FaultMode::Throw: return "fail";
    case FaultMode::Hang: return "hang";
    case FaultMode::Exit: return "exit the process";
    case FaultMode::Freeze: return "freeze the worker";
    case FaultMode::Corrupt: return "corrupt its tiles";
    case FaultMode::Upload: return "fail to take the scene";
  }
  return "?";
}

bool testFrozen() { return g_frozen.load(); }

bool testTakeCorruptTile() {
  uint32_t n = g_corrupt_tiles.load();
  while (n > 0)
    if (g_corrupt_tiles.compare_exchange_weak(n, n - 1)) return true;
  return false;
}

std::unique_ptr<IBackend> wrapWithFault(std::unique_ptr<IBackend> backend, const FaultSpec& spec) {
  return std::make_unique<FaultyBackend>(std::move(backend), spec);
}

}  // namespace pen
