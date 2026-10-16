#pragma once

#include <memory>

#include "backends/backend.h"

namespace pen {

// CPU backend: Embree 4 two-level BVH (one BLAS per mesh, instances on top) and a oneTBB thread arena.
// threads = 0 means all hardware threads.
std::unique_ptr<IBackend> createCpuBackend(uint32_t threads);

}  // namespace pen
