#pragma once

// The scene flattened into the arrays the GPU shaders read (shaders/scene.slang, lights.slang), shared by the
// Vulkan and CUDA backends so both upload the same bytes.

#include <cstdint>
#include <vector>

#include "backends/backend.h"
#include "backends/gpu/gpu_types.h"
#include "core/math/vec.h"

namespace pen {

struct GpuScene {
  std::vector<gpu::Instance> instances;
  std::vector<gpu::Primitive> primitives;
  std::vector<uint32_t> mesh_primitive_base;  // first primitive of each mesh
  std::vector<Vec3f> positions, normals;
  std::vector<Vec4f> tangents, colors;
  std::vector<Vec2f> uvs;
  std::vector<uint32_t> indices;
  std::vector<gpu::Material> materials;
  std::vector<gpu::Texture> textures;
  std::vector<uint32_t> texels8;
  std::vector<Vec4f> texels32;
  std::vector<gpu::EmissiveTriangle> emissive;
  std::vector<int32_t> emissive_offset;
  std::vector<gpu::AliasEntry> light_table;
  std::vector<gpu::PunctualLight> lights;
  std::vector<gpu::AliasEntry> env_tables;
  std::vector<float> sheen_table, srgb_table;
  // Large arrays used as they are (they belong to the Scene, which outlives the upload).
  const std::vector<Vec3f>* env_map = nullptr;
  const std::vector<float>* env_weights = nullptr;
};

GpuScene flattenScene(const Scene& scene);
// (Re)fills g.textures, g.texels8 and g.texels32, each texture halved halvings[i] times (memory tier 2; empty: full
// size). flattenScene packs them at full size.
void packTextures(const Scene& scene, const std::vector<uint32_t>& halvings, GpuScene& g);

// The per-render constants (camera, integrator, light and environment summaries).
gpu::Frame makeGpuFrame(const FrameSetup& frame, const Scene& scene);

}  // namespace pen
