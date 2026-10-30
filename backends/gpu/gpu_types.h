#pragma once

// Scene and frame data as the GPU backends read it. shaders/types.slang declares the same structs.
//
// One byte layout serves both GPU backends (DECISIONS D-086): Vulkan reads it with the scalar block layout (no
// implicit padding) and CUDA with C's natural alignment (float2 on 8 bytes, float4 on 16). Fields are ordered so
// the two agree: every float2 starts on a multiple of 8, every float4 on a multiple of 16, and each struct's size
// is padded by hand to its alignment. The CUDA backend checks every offset below against the layout slangc
// reports for the CUDA kernels (static_asserts in backends/cuda/cuda_backend.cpp), and the GPU parity and golden
// image tests catch any remaining mismatch.

#include <cstdint>

namespace pen::gpu {

struct TextureRef {
  int32_t index = -1;
  uint32_t tex_coord = 0;
  float offset[2] = {0, 0};  // float2: offset 8
  uint32_t has_transform = 0;
  float rotation = 0;
  float scale[2] = {1, 1};   // float2: offset 24
};
static_assert(sizeof(TextureRef) == 32);

struct Material {
  float base_color[4];  // float4: offset 0
  TextureRef base_color_tex;
  TextureRef metallic_roughness_tex;
  TextureRef normal_tex;
  TextureRef emissive_tex;
  TextureRef transmission_tex;
  TextureRef specular_tex;
  TextureRef specular_color_tex;
  TextureRef clearcoat_tex;
  TextureRef clearcoat_roughness_tex;
  TextureRef sheen_color_tex;
  TextureRef sheen_roughness_tex;
  TextureRef anisotropy_tex;
  float metallic, roughness;
  float normal_scale;
  float emissive[3];
  float emissive_strength;
  float transmission;
  float ior, specular;
  float specular_color[3];
  float clearcoat;
  float clearcoat_roughness;
  float sheen_color[3];
  float sheen_roughness;
  float anisotropy, anisotropy_rotation;
  uint32_t alpha_mode;  // 0 opaque, 1 mask, 2 blend
  float alpha_cutoff;
  uint32_t double_sided;
  uint32_t emits;
  uint32_t pad[3];  // size to a multiple of 16 (float4 alignment)
};
static_assert(sizeof(Material) == 512);

struct Texture {
  uint32_t width, height;
  uint32_t is_float;
  uint32_t wrap_s, wrap_t;  // 0 repeat, 1 clamp, 2 mirror
  uint32_t nearest;
  uint32_t offset;  // into its RGBA8 texel buffer (uint per texel) or float4 texel buffer
  uint32_t part;    // which of the kTexelParts buffers of its kind holds it (D-109)
};
static_assert(sizeof(Texture) == 32);

// One glTF primitive. Vertex attributes are indexed attribute_offset + local vertex index; -1 = absent.
struct Primitive {
  uint32_t index_offset;
  uint32_t position_offset;
  int32_t normal_offset, tangent_offset, uv0_offset, uv1_offset, color_offset;
  uint32_t material;
};
static_assert(sizeof(Primitive) == 32);

struct Instance {
  float to_world[12];  // row-major 3x4
  float inverse[12];   // of to_world: rays into object space (CUDA) and normals (inverse transpose)
  uint32_t mesh;
  uint32_t primitive_base;  // first Primitive of the mesh
  uint32_t slot_base;       // first entry of this instance's primitives in the emissive offset table
};
static_assert(sizeof(Instance) == 4 * 27);

struct EmissiveTriangle {
  float p0[3], e1[3], e2[3], normal[3];
  float area;
  uint32_t instance, primitive, triangle;
  uint32_t double_sided;
};
static_assert(sizeof(EmissiveTriangle) == 4 * 17);

struct AliasEntry {
  float prob;
  uint32_t alias;
  float pmf;
};
static_assert(sizeof(AliasEntry) == 12);

struct PunctualLight {
  uint32_t type;  // 0 point, 1 spot, 2 directional
  float position[3], direction[3], intensity[3];
  float cos_inner, cos_outer;
};
static_assert(sizeof(PunctualLight) == 4 * 12);

// One exponential height layer of the atmosphere (core/scene/scene.h MediumLayer, M14).
struct MediumLayer {
  float sigma_s[3], sigma_t[3];
  float base, inv_height, g;
  uint32_t rayleigh;
};
static_assert(sizeof(MediumLayer) == 4 * 10);

struct Frame {
  // Camera
  uint32_t projection;  // 0 perspective, 1 orthographic, 2 equirectangular
  float cam_to_world[12];
  float ymag, aperture_radius, focus_distance;
  float width, height, tan_x, tan_y;
  // Integrator
  uint32_t mode;  // Integrator enum
  uint32_t max_bounces, max_diffuse, max_glossy, max_transmission;
  uint32_t russian_roulette;
  float ao_distance;
  uint32_t seed;
  // Lights
  uint32_t num_emissive, num_punctual, light_count;
  float light_sum;
  // Environment
  uint32_t env_has_map, env_width, env_height;
  float env_strength, env_rotation;
  float env_color[3];
  uint32_t env_is_black;
  float env_dist_sum;
  uint32_t env_light_index;
  // Atmosphere (M14): medium_count layers, 0 without one
  uint32_t medium_count;
  MediumLayer medium[3];
};
static_assert(sizeof(Frame) == 4 * (44 + 30));

// Per launch: Vulkan push constants, CUDA kernel argument. A work item may be split into several launches over
// consecutive sample ranges (CUDA, to keep each launch short); the running sum stays in the output buffer, so the
// result is the same bits as one launch.
struct PushConstants {
  uint32_t x0, y0, width, height;
  uint32_t sample_begin, sample_end;  // this launch's samples
  uint32_t sample_count;              // the work item's samples (the divisor of the mean)
  uint32_t flags;                     // kItem* below
};
static_assert(sizeof(PushConstants) == 32);
constexpr uint32_t kItemAccumulate = 1;  // start from the sum in the output buffer instead of zero
constexpr uint32_t kItemFinish = 2;      // write the mean (sum / sample_count) instead of the sum
constexpr uint32_t kItemPassShift = 8;   // bits 8-15: the item's Pass (0 the image; an AOV renders its debug view)

// ---- Software ray tracing (CUDA backend; shaders/bvh.slang) ----

// One mesh's bottom-level structure. kind: 0 no triangles, 1 CWBVH (tinybvh BVH8_CWBVH: 5 float4 per node,
// 3 float4 per triangle), 2 triangle list (meshes of 1 to 3 triangles, too small for a CWBVH).
struct Blas {
  uint32_t kind;
  uint32_t node_base;  // first float4 of the mesh's nodes
  uint32_t tri_base;   // first float4 of the mesh's triangles
  uint32_t tri_count;
};
static_assert(sizeof(Blas) == 16);

// Top-level BVH over the scene instances (binary). Leaf: count > 0 instances from tlas_index[first].
// Inner: count == 0, children at left_first and left_first + 1.
struct TlasNode {
  float lo[3];
  uint32_t left_first;
  float hi[3];
  uint32_t count;
};
static_assert(sizeof(TlasNode) == 32);

}  // namespace pen::gpu
