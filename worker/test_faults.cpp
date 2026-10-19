#include "worker/test_faults.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "core/settings/parse.h"

namespace pen {

namespace {

class FaultyBackend final : public IBackend {
 public:
  FaultyBackend(std::unique_ptr<IBackend> inner, const FaultSpec& spec) : inner_(std::move(inner)), spec_(spec) {}
  DeviceInfo info() const override { return inner_->info(); }
  MemoryPlan plan(const SceneStats& s) override { return inner_->plan(s); }
  void upload(const Scene& scene, const MemoryPlan& plan) override { inner_->upload(scene, plan); }
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
      if (!spec_.hang) throw BackendError("injected test fault (PENUMBRA_TEST_FAULTS)");
      while (!cancel_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      out.item_id = item.id;
      out.cancelled = true;
      return;
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
    else if (key == "mode" && (value == "throw" || value == "hang")) spec.hang = value == "hang";
    else throw std::invalid_argument("PENUMBRA_TEST_FAULTS: use device=N,after=K,mode=throw|hang");
    if (comma == std::string_view::npos) break;
    s.remove_prefix(comma + 1);
  }
  return spec;
}

std::unique_ptr<IBackend> wrapWithFault(std::unique_ptr<IBackend> backend, const FaultSpec& spec) {
  return std::make_unique<FaultyBackend>(std::move(backend), spec);
}

}  // namespace pen
