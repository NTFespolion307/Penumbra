// GPU scene and frame data; must match backends/gpu/gpu_types.h field for field (one layout for Vulkan scalar
// block layout and CUDA natural alignment, DECISIONS D-086).
#pragma once

struct TextureRef {
  int index;
  uint tex_coord;
  float2 offset;
  uint has_transform;
  float rotation;
  float2 scale;
};

struct Material {
  float4 base_color;
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
  float metallic;
  float roughness;
  float normal_scale;
  float3 emissive;
  float emissive_strength;
  float transmission;
  float ior;
  float specular;
  float3 specular_color;
  float clearcoat;
  float clearcoat_roughness;
  float3 sheen_color;
  float sheen_roughness;
  float anisotropy;
  float anisotropy_rotation;
  uint alpha_mode;
  float alpha_cutoff;
  uint double_sided;
  uint emits;
  uint pad0;
  uint pad1;
  uint pad2;
};

struct Texture {
  uint width;
  uint height;
  uint is_float;
  uint wrap_s;
  uint wrap_t;
  uint nearest;
  uint offset;
  uint part;
};

struct Primitive {
  uint index_offset;
  uint position_offset;
  int normal_offset;
  int tangent_offset;
  int uv0_offset;
  int uv1_offset;
  int color_offset;
  uint material;
};

struct Instance {
  float to_world[12];
  float inverse[12];
  uint mesh;
  uint primitive_base;
  uint slot_base;
};

struct EmissiveTriangle {
  float3 p0;
  float3 e1;
  float3 e2;
  float3 normal;
  float area;
  uint instance;
  uint primitive;
  uint triangle;
  uint double_sided;
};

struct AliasEntry {
  float prob;
  uint alias;
  float pmf;
};

struct PunctualLight {
  uint type;
  float3 position;
  float3 direction;
  float3 intensity;
  float cos_inner;
  float cos_outer;
};

// One exponential height layer of the atmosphere (backends/gpu/gpu_types.h MediumLayer, M14).
struct MediumLayer {
  float3 sigma_s;
  float3 sigma_t;
  float base;
  float inv_height;
  float g;
  uint rayleigh;
};

struct FrameData {
  uint projection;
  float cam_to_world[12];
  float ymag;
  float aperture_radius;
  float focus_distance;
  float width;
  float height;
  float tan_x;
  float tan_y;
  uint mode;
  uint max_bounces;
  uint max_diffuse;
  uint max_glossy;
  uint max_transmission;
  uint russian_roulette;
  float ao_distance;
  uint seed;
  uint num_emissive;
  uint num_punctual;
  uint light_count;
  float light_sum;
  uint env_has_map;
  uint env_width;
  uint env_height;
  float env_strength;
  float env_rotation;
  float3 env_color;
  uint env_is_black;
  float env_dist_sum;
  uint env_light_index;
  uint medium_count;
  MediumLayer medium[3];
};

struct PushConstants {
  uint x0;
  uint y0;
  uint width;
  uint height;
  uint sample_begin;
  uint sample_end;
  uint sample_count;
  uint flags;
};
static const uint kItemAccumulate = 1;
static const uint kItemFinish = 2;

// Software ray tracing (CUDA backend, bvh.slang).
struct Blas {
  uint kind;  // 0 no triangles, 1 CWBVH, 2 triangle list
  uint node_base;
  uint tri_base;
  uint tri_count;
};

struct TlasNode {
  float3 lo;
  uint left_first;
  float3 hi;
  uint count;
};

// Integrator modes (core/settings/render_settings.h Integrator).
static const uint kModePath = 0;
static const uint kModeAo = 1;
static const uint kModeDebugNormal = 2;
static const uint kModeDebugAlbedo = 3;
static const uint kModeDebugDepth = 4;
static const uint kModeDebugBvh = 5;
