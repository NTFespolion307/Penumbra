#pragma once

#include "core/math/vec.h"
#include "core/render/surface.h"
#include "core/scene/scene.h"

namespace pen {

struct LightSample {
  Vec3f wi;           // direction from the shading point to the light
  float dist = 0;     // to the light point; infinity for directional and environment lights
  Vec3f L{0, 0, 0};   // incident radiance (area, environment) or intensity / d^2 (punctual)
  float pdf = 0;      // solid-angle pdf including light selection; for delta lights the selection pmf
  bool delta = false; // punctual light: no MIS
  Vec3f p_light;      // point on the light (area lights), for the shadow ray end
  Vec3f n_light;      // its normal (area lights)
};

// Picks a light with the power alias table and samples a point / direction on it as seen from p.
bool sampleLight(const Scene& scene, Vec3f p, float uSelect, Vec2f u, LightSample& out);

// Radiance arriving from the environment along direction d (world, pointing away from the scene).
Vec3f environmentRadiance(const Environment& env, Vec3f d);

// Solid-angle pdf (incl. selection) that sampleLight produces direction d toward the environment.
float environmentPdf(const Scene& scene, Vec3f d);

// Solid-angle pdf (incl. selection) of reaching the emissive point hit at `si` from `from`.
float emissiveHitPdf(const Scene& scene, const Hit& hit, Vec3f from, const SurfaceInteraction& si);

}  // namespace pen
