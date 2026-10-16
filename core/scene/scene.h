#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/image/image.h"
#include "core/math/transform.h"
#include "core/sampling/alias_table.h"

namespace pen {

// Texture slot with glTF texCoord set and KHR_texture_transform.
struct TextureRef {
  int32_t index = -1;  // into Scene::textures, -1 = none
  uint32_t tex_coord = 0;
  bool has_transform = false;
  Vec2f offset{0, 0};
  float rotation = 0;
  Vec2f scale{1, 1};

  bool valid() const { return index >= 0; }
  Vec2f apply(Vec2f uv) const {
    if (!has_transform) return uv;
    const float c = std::cos(rotation), s = std::sin(rotation);
    // KHR_texture_transform: T * R * S, rotation counter-clockwise in UV space.
    const Vec2f sc{uv.x * scale.x, uv.y * scale.y};
    return {c * sc.x + s * sc.y + offset.x, -s * sc.x + c * sc.y + offset.y};
  }
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

// glTF metallic-roughness material plus the extensions listed in docs/GUIDE.md.
struct Material {
  std::string name;
  Vec4f base_color{1, 1, 1, 1};
  TextureRef base_color_tex;
  float metallic = 1, roughness = 1;
  TextureRef metallic_roughness_tex;  // G = roughness, B = metallic
  TextureRef normal_tex;
  float normal_scale = 1;
  Vec3f emissive{0, 0, 0};
  float emissive_strength = 1;
  TextureRef emissive_tex;
  float transmission = 0;
  TextureRef transmission_tex;  // R
  float ior = 1.5f;
  float specular = 1;
  TextureRef specular_tex;  // A
  Vec3f specular_color{1, 1, 1};
  TextureRef specular_color_tex;
  float clearcoat = 0;
  TextureRef clearcoat_tex;  // R
  float clearcoat_roughness = 0;
  TextureRef clearcoat_roughness_tex;  // G
  Vec3f sheen_color{0, 0, 0};
  TextureRef sheen_color_tex;
  float sheen_roughness = 0;
  TextureRef sheen_roughness_tex;  // A
  float anisotropy = 0;
  float anisotropy_rotation = 0;
  TextureRef anisotropy_tex;  // RG direction, B strength
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;
  bool double_sided = false;

  bool emits() const { return emissive_strength > 0 && !isBlack(emissive); }
  bool usesAlpha() const { return alpha_mode != AlphaMode::Opaque; }
};

// One glTF primitive: a triangle list with one material. One Embree geometry.
struct MeshPrimitive {
  std::vector<Vec3f> positions;
  std::vector<Vec3f> normals;   // empty: flat shading
  std::vector<Vec4f> tangents;  // xyz + handedness w; empty: none
  std::vector<Vec2f> uv0, uv1;
  std::vector<Vec4f> colors;     // COLOR_0, multiplies base colour; empty: none
  std::vector<uint32_t> indices;  // 3 per triangle
  uint32_t material = 0;

  uint32_t triangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
};

struct Mesh {
  std::string name;
  std::vector<MeshPrimitive> primitives;
};

struct Instance {
  std::string name;
  uint32_t mesh = 0;
  Affine3f to_world;
};

enum class CameraProjection : uint8_t { Perspective, Orthographic, Equirectangular };

// Camera looking down its local -Z with +Y up (glTF convention).
struct Camera {
  std::string name;
  CameraProjection projection = CameraProjection::Perspective;
  Affine3f to_world;
  float yfov = 0.6911f;       // radians, perspective
  float xmag = 1, ymag = 1;   // half extents, orthographic
  float aperture_radius = 0;  // scene units; 0 = pinhole
  float focus_distance = 1;
};

enum class LightType : uint8_t { Point, Spot, Directional };

// KHR_lights_punctual light in world space. `intensity` is radiometric: W/sr for point and spot, W/m^2 for
// directional (glTF photometric values divided by 683 lm/W, DECISIONS D-023).
struct PunctualLight {
  std::string name;
  LightType type = LightType::Point;
  Vec3f position{0, 0, 0};
  Vec3f direction{0, 0, -1};  // direction the light travels
  Vec3f intensity{1, 1, 1};
  float cos_inner = 1, cos_outer = 0;  // spot cone
};

struct Environment {
  Vec3f color{0, 0, 0};    // uniform radiance when there is no map (already times strength)
  float strength = 1;      // applies to the map
  float rotation = 0;      // radians around the up axis
  ImageRGB map;            // equirectangular, empty if none
  Distribution2D sampling; // over map pixels, weighted by luminance * sin(theta)
  std::string name;

  bool hasMap() const { return map.width > 0; }
  bool isBlack() const { return !hasMap() && pen::isBlack(color); }
};

// A triangle with emission, in world space, for light sampling.
struct EmissiveTriangle {
  Vec3f p0, e1, e2;  // p0 and edges p1 - p0, p2 - p0
  Vec3f normal;      // unit geometric normal (front face)
  float area = 0;
  uint32_t instance = 0, primitive = 0, triangle = 0;
  bool double_sided = false;
};

struct SceneStats {
  uint64_t triangles = 0;           // after instancing
  uint64_t unique_triangles = 0;    // before instancing
  uint32_t instances = 0, meshes = 0, materials = 0, textures = 0, punctual_lights = 0;
  uint64_t emissive_triangles = 0;
  uint64_t geometry_bytes = 0, texture_bytes = 0;
};

struct Scene {
  std::vector<Mesh> meshes;
  std::vector<Instance> instances;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  std::vector<Camera> cameras;
  std::vector<PunctualLight> lights;
  Environment environment;
  Bounds3f bounds;  // world space

  // Light sampling, built by finalize().
  std::vector<EmissiveTriangle> emissive_triangles;
  // emissive_offset[instance][primitive] = index of the primitive's first triangle in emissive_triangles, or -1.
  std::vector<std::vector<int32_t>> emissive_offset;
  AliasTable light_table;  // entries: emissive triangles, then punctual lights, then the environment (if any)

  // Fills bounds, emissive triangles and the light table. Call after loading.
  void finalize();

  uint32_t lightCount() const { return light_table.size(); }
  uint32_t environmentLightIndex() const {
    return static_cast<uint32_t>(emissive_triangles.size() + lights.size());
  }
  SceneStats stats() const;
  float boundingRadius() const;
};

}  // namespace pen
