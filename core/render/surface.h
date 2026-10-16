#pragma once

#include <cstdint>

#include "core/math/vec.h"
#include "core/scene/scene.h"
#include "core/shading/bsdf.h"

namespace pen {

// Result of a ray query, as returned by every backend.
struct Hit {
  bool valid = false;
  float t = 0;
  uint32_t instance = 0, primitive = 0, triangle = 0;
  float u = 0, v = 0;  // barycentrics of vertices 1 and 2
};

// Everything the integrator needs at a hit point, in world space.
struct SurfaceInteraction {
  Vec3f p;
  Vec3f ng;        // geometric normal, front face (glTF winding)
  Vec3f ns;        // shading normal (interpolated + normal map), same side as ng
  Frame frame;     // shading frame around ns; tangent = anisotropy direction
  Vec2f uv0, uv1;
  uint32_t material = 0;
  BsdfInputs bsdf;
  Vec3f emission{0, 0, 0};  // radiance leaving the front face
  float alpha = 1;
  bool double_sided = false;
  bool front_face = true;   // the ray arrived on the front side
};

// Interpolates geometry and evaluates every material texture at the hit. `wo` points back along the ray.
SurfaceInteraction makeSurfaceInteraction(const Scene& scene, const Hit& hit, Vec3f wo);

// Alpha at a hit, for cutout tests inside traversal (cheap: only base colour alpha).
float alphaAt(const Scene& scene, uint32_t instance, uint32_t primitive, uint32_t triangle, float u, float v);

// Offsets a ray origin off a surface along n, robust at any scale (Wächter & Binder, Ray Tracing Gems ch. 6).
Vec3f offsetRayOrigin(Vec3f p, Vec3f n);

}  // namespace pen
