#pragma once

// Acceleration structures for software ray tracing on the GPU (CUDA backend, shaders/bvh.slang): one tinybvh
// CWBVH per mesh and a binary top-level BVH over the instances, built on the CPU (DECISIONS D-086).

#include <cstdint>
#include <string>
#include <vector>

#include "backends/gpu/gpu_types.h"
#include "core/math/vec.h"
#include "core/scene/scene.h"

namespace pen {

struct GpuBvh {
  std::vector<Vec4f> nodes;             // g_bvh_nodes
  std::vector<Vec4f> tris;              // g_bvh_tris
  std::vector<gpu::Blas> blas;          // per mesh
  std::vector<gpu::TlasNode> tlas;      // g_tlas_nodes (node 0 = root)
  std::vector<uint32_t> tlas_index;     // g_tlas_index
  std::vector<uint32_t> tri_map;        // g_tri_map: (primitive, triangle) pairs, two uints per triangle id
  std::vector<std::string> notes;       // automatic decisions worth logging
};

// Throws BackendError when tinybvh rejects a mesh.
GpuBvh buildGpuBvh(const Scene& scene);

}  // namespace pen
