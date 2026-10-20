#pragma once

// Scene and frame data as the Vulkan shaders read it (scalar block layout: every field 4-byte aligned, so these
// structs are copied to GPU buffers as they are). shaders/types.slang declares the same structs; a static_assert
// on each size and the GPU parity test catch any mismatch.

#include <cstdint>

namespace pen::gpu {

struct TextureRef {
  int32_t index = -1;
  uint32_t tex_coord = 0;
  uint32_t has_transform = 0;
  float offset[2] = {0, 0};
  float rotation = 0;
  float scale[2] = {1, 1};
};
static_assert(sizeof(TextureRef) == 32);

struct Material {
  float base_color[4];
  TextureRef base_color_tex;
  float metallic, roughness;
  TextureRef metallic_roughness_tex;
  TextureRef normal_tex;
  float normal_scale;
  float emissive[3];
  float emissive_strength;
  TextureRef emissive_tex;
  float transmission;
  TextureRef transmission_tex;
  float ior, specular;
  TextureRef specular_tex;
  float specular_color[3];
  TextureRef specular_color_tex;
  float clearcoat;
  TextureRef clearcoat_tex;
  float clearcoat_roughness;
  TextureRef clearcoat_roughness_tex;
  float sheen_color[3];
  TextureRef sheen_color_tex;
  float sheen_roughness;
  TextureRef sheen_roughness_tex;
  float anisotropy, anisotropy_rotation;
  TextureRef anisotropy_tex;
  uint32_t alpha_mode;  // 0 opaque, 1 mask, 2 blend
  float alpha_cutoff;
  uint32_t double_sided;
  uint32_t emits;
};
static_assert(sizeof(Material) == 4 * 29 + 32 * 12);

struct Texture {
  uint32_t width, height;
  uint32_t is_float;
  uint32_t wrap_s, wrap_t;  // 0 repeat, 1 clamp, 2 mirror
  uint32_t nearest;
  uint32_t offset;  // into the RGBA8 texel buffer (uint per texel) or the float4 texel buffer
};
static_assert(sizeof(Texture) == 28);

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
  float inverse[12];   // of to_world, for normals (inverse transpose of the linear part)
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
};
static_assert(sizeof(Frame) == 4 * 43);

// Per work item.
struct PushConstants {
  uint32_t x0, y0, width, height;
  uint32_t sample_begin, sample_end;
};

}  // namespace pen::gpu
