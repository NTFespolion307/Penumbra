// GPU scene and frame data; must match backends/vulkan/gpu_types.h field for field (scalar block layout).
#pragma once

struct TextureRef {
  int index;
  uint tex_coord;
  uint has_transform;
  float2 offset;
  float rotation;
  float2 scale;
};

struct Material {
  float4 base_color;
  TextureRef base_color_tex;
  float metallic;
  float roughness;
  TextureRef metallic_roughness_tex;
  TextureRef normal_tex;
  float normal_scale;
  float3 emissive;
  float emissive_strength;
  TextureRef emissive_tex;
  float transmission;
  TextureRef transmission_tex;
  float ior;
  float specular;
  TextureRef specular_tex;
  float3 specular_color;
  TextureRef specular_color_tex;
  float clearcoat;
  TextureRef clearcoat_tex;
  float clearcoat_roughness;
  TextureRef clearcoat_roughness_tex;
  float3 sheen_color;
  TextureRef sheen_color_tex;
  float sheen_roughness;
  TextureRef sheen_roughness_tex;
  float anisotropy;
  float anisotropy_rotation;
  TextureRef anisotropy_tex;
  uint alpha_mode;
  float alpha_cutoff;
  uint double_sided;
  uint emits;
};

struct Texture {
  uint width;
  uint height;
  uint is_float;
  uint wrap_s;
  uint wrap_t;
  uint nearest;
  uint offset;
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
};

struct PushConstants {
  uint x0;
  uint y0;
  uint width;
  uint height;
  uint sample_begin;
  uint sample_end;
};

// Integrator modes (core/settings/render_settings.h Integrator).
static const uint kModePath = 0;
static const uint kModeAo = 1;
static const uint kModeDebugNormal = 2;
static const uint kModeDebugAlbedo = 3;
static const uint kModeDebugDepth = 4;
