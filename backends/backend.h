#pragma once

// IBackend: what one render device implements (docs/GUIDE.md "Repository layout and core interfaces").

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/render/camera.h"
#include "core/render/integrator.h"
#include "core/render/work.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

struct DeviceInfo {
  std::string name;          // e.g. "11th Gen Intel(R) Core(TM) i7-11850H"
  BackendKind backend = BackendKind::Cpu;
  uint32_t threads = 0;      // CPU worker threads
  uint64_t memory_bytes = 0; // device memory (RAM for the CPU)
  bool hardware_rt = false;
  std::array<uint8_t, 16> uuid{};  // GPUs: Vulkan deviceUUID / CUDA UUID (NVML readings, D-093); zero otherwise
};

// Where the scene lives on the device (memory tiers arrive in M8; M1 is always tier 0).
struct MemoryPlan {
  uint32_t tier = 0;
  std::string summary;
};

// Per-render state every device needs besides the scene.
struct FrameSetup {
  RenderCamera camera;
  IntegratorParams params;
  uint32_t seed = 0;
};

struct BackendError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual DeviceInfo info() const = 0;
  virtual MemoryPlan plan(const SceneStats& stats) = 0;
  // Builds acceleration structures; the scene must outlive the backend's use of it.
  virtual void upload(const Scene& scene, const MemoryPlan& plan) = 0;
  virtual void prepare(const FrameSetup& frame) = 0;
  // Renders item.rect for samples [sample_begin, sample_end) into out (sized by the backend).
  virtual void render(const WorkItem& item, TileResult& out) = 0;
  // Asks render() calls in progress (and later ones, until the next prepare()) to stop early and return a
  // result marked cancelled. Thread-safe.
  virtual void cancel() = 0;
};

}  // namespace pen
