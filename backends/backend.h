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
#include "core/render/memory_placement.h"
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

// Per-render state every device needs besides the scene.
struct FrameSetup {
  RenderCamera camera;
  IntegratorParams params;
  uint32_t seed = 0;
};

struct BackendError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// An allocation failed for lack of VRAM or pinned RAM: the worker tries the next memory tier instead of giving the
// device up (docs/GUIDE.md "no out-of-memory crash, ever").
struct OutOfMemoryError : BackendError {
  using BackendError::BackendError;
};

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual DeviceInfo info() const = 0;
  // Usable VRAM now: free VRAM minus a 10% margin (docs/GUIDE.md "Budget"). 0 for devices without memory of their
  // own (the CPU), which keep the scene where it already is: always tier 0.
  virtual uint64_t vramBudget() { return 0; }
  // The exact bytes this device needs for `scene` (docs/GUIDE.md "Scene placement ladder"). May keep work for
  // upload() (flattened arrays, a BVH), which then follows for the same scene.
  virtual SceneFootprint measure(const Scene& scene) {
    (void)scene;
    return {};
  }
  // Puts the scene where `placement` says and builds acceleration structures; the scene must outlive the backend's
  // use of it.
  virtual void upload(const Scene& scene, const Placement& placement) = 0;
  virtual void prepare(const FrameSetup& frame) = 0;
  // Renders item.rect for samples [sample_begin, sample_end) into out (sized by the backend).
  virtual void render(const WorkItem& item, TileResult& out) = 0;
  // Asks render() calls in progress (and later ones, until the next prepare()) to stop early and return a
  // result marked cancelled. Thread-safe.
  virtual void cancel() = 0;
};

}  // namespace pen
