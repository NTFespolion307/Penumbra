#pragma once

// CUDA compute backend (docs/GUIDE.md "Rendering backends"): the shared Slang path tracer compiled to CUDA, rays
// traced in software through tinybvh CWBVHs, for NVIDIA GPUs without ray tracing hardware (and on request,
// --backend cuda, for any NVIDIA GPU). The CUDA driver is loaded at run time (nvcuda.dll / libcuda.so.1), so a
// machine without one still runs Penumbra. This header has no CUDA types.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backends/backend.h"

namespace pen {

struct CudaGpu {
  uint32_t ordinal = 0;  // CUDA device ordinal
  std::string name;
  int cc_major = 0, cc_minor = 0;  // compute capability
  uint64_t vram_bytes = 0;
  std::array<uint8_t, 16> uuid{};  // matches Vulkan's deviceUUID for the same GPU
  bool usable = false;             // this build has kernels this GPU and driver can run
  std::string why;                 // why not, when it is not
};

struct CudaProbe {
  bool driver_found = false;
  int driver_version = 0;  // e.g. 12080 = CUDA 12.8
  std::string error;       // why probing failed, if it did
  std::vector<CudaGpu> gpus;
};

// Loads the CUDA driver and lists the GPUs. Never throws.
const CudaProbe& probeCuda();

// What this build contains, e.g. "CUDA 13.0 kernels: sm_75 sm_86, PTX compute_75".
std::string cudaBuildSummary();

// A backend on one CUDA GPU (CudaGpu::ordinal). Throws BackendError.
std::unique_ptr<IBackend> createCudaBackend(uint32_t ordinal);

// Test only (tests/gpu/parity.cpp): runs shaders/parity.slang on a CUDA GPU (32 floats per case).
std::vector<float> runCudaParity(uint32_t ordinal, uint32_t count);

}  // namespace pen
