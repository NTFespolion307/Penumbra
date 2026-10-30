#pragma once

// Light transport, written once and instantiated by each backend with its own Tracer:
//   struct Tracer {
//     Hit intersect(Vec3f origin, Vec3f dir, float tmax) const;
//     bool occluded(Vec3f origin, Vec3f dir, float tmax) const;
//   };
// `path`: unidirectional path tracing with next-event estimation, MIS (power heuristic) and Russian roulette
// after bounce 3. `ao` and `debug:*` are single-hit views.

#include <cmath>
#include <limits>

#include "core/render/lights.h"
#include "core/render/medium.h"
#include "core/render/surface.h"
#include "core/sampling/sampler.h"
#include "core/sampling/warp.h"
#include "core/settings/render_settings.h"

namespace pen {

struct IntegratorParams {
  Integrator mode = Integrator::Path;
  uint32_t max_bounces = 12;
  uint32_t max_diffuse = 12, max_glossy = 12, max_transmission = 12;
  bool russian_roulette = true;
  float ao_distance = 1;
  bool raw_normals = false;  // debug:normal as the normal AOV pass wants it: n in [-1, 1], 0 where nothing is hit

  static IntegratorParams from(const RenderSettings& s, const Scene& scene) {
    IntegratorParams p;
    p.mode = s.integrator;
    p.max_bounces = s.max_bounces;
    p.max_diffuse = s.max_diffuse.value_or(s.max_bounces);
    p.max_glossy = s.max_glossy.value_or(s.max_bounces);
    p.max_transmission = s.max_transmission.value_or(s.max_bounces);
    p.russian_roulette = !s.no_rr;
    p.ao_distance = s.ao_distance > 0 ? static_cast<float>(s.ao_distance) : 0.2f * scene.boundingRadius();
    return p;
  }
};

// Sampler dimension layout: 0 = pixel jitter, 1 = lens; then kDimsPerBounce per bounce:
// light select, light point, BSDF lobe, BSDF direction, Russian roulette.
inline constexpr uint32_t kCameraDims = 2;
inline constexpr uint32_t kDimsPerBounce = 5;
// The atmosphere (M14) draws from its own range, so renders without one keep their exact numbers: per bounce the
// colour channel, one distance per layer, then at a scattering point light select, light point, phase direction,
// Russian roulette.
inline constexpr uint32_t kMediumDims = 0x10000;
inline constexpr uint32_t kMediumDimsPerBounce = 8;

namespace detail {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Orients the hit for shading: non-transmissive surfaces face the incoming ray; transmissive ones keep their
// geometric side so the BSDF can tell inside from outside. Falls back to the geometric normal when the
// shading normal faces away from wo.
inline void orientForShading(SurfaceInteraction& si, Vec3f wo) {
  const bool transmissive = si.bsdf.transmission > 0 && si.bsdf.metallic < 1;
  if (!transmissive && dot(wo, si.ng) < 0) {
    si.ng = -si.ng;
    si.ns = -si.ns;
    si.frame = Frame::fromNormalTangent(si.ns, si.frame.t);
  }
  if (dot(wo, si.ns) * dot(wo, si.ng) <= 0) {
    si.ns = si.ng;
    si.frame = Frame::fromNormalTangent(si.ns, si.frame.t);
  }
}

// What reaches o from the light sample: 0 when blocked, else the atmosphere's transmittance (1 without one).
template <class Tracer>
Vec3f shadowTransmittance(const Scene& scene, const Tracer& tracer, Vec3f o, const LightSample& ls) {
  if (std::isinf(ls.dist)) {
    if (tracer.occluded(o, ls.wi, kInf)) return Vec3f(0);
    return scene.medium.empty() ? Vec3f(1) : mediumTransmittance(scene.medium, o, ls.wi, kInf);
  }
  const Vec3f target = offsetRayOrigin(ls.p_light, dot(ls.n_light, -ls.wi) >= 0 ? ls.n_light : -ls.n_light);
  const Vec3f toTarget = (ls.delta ? ls.p_light : target) - o;
  const float d = length(toTarget);
  if (tracer.occluded(o, toTarget / d, d * (1 - 1e-4f))) return Vec3f(0);
  return scene.medium.empty() ? Vec3f(1) : mediumTransmittance(scene.medium, o, toTarget / d, d);
}

}  // namespace detail

template <class Tracer>
Vec3f tracePath(const Scene& scene, const Tracer& tracer, const IntegratorParams& params, Vec3f origin, Vec3f dir,
                Sampler& sampler) {
  Vec3f L(0), beta(1);
  bool specular = true;  // previous bounce was a delta lobe (or the camera)
  float prevPdf = 0;
  Vec3f prevP = origin;
  uint32_t nDiffuse = 0, nGlossy = 0, nTrans = 0;
  bool lastBounce = false;

  for (uint32_t depth = 0;; ++depth) {
    const Hit hit = tracer.intersect(origin, dir, detail::kInf);

    // The atmosphere: the path may scatter before it reaches the surface (or the sky).
    if (!scene.medium.empty()) {
      sampler.setDimension(kMediumDims + depth * kMediumDimsPerBounce);
      const float uChannel = sampler.next1D();
      float uDist[Medium::kMaxLayers];
      for (float& u : uDist) u = sampler.next1D();
      const MediumEvent ev =
          sampleMedium(scene.medium, origin, dir, hit.valid ? hit.t : detail::kInf, uChannel, uDist);
      beta *= ev.weight;
      if (isBlack(beta) || !isFinite(beta)) break;
      if (ev.scattered) {
        if (lastBounce) break;  // no further scattering; what lies beyond counts in the unscattered branch
        const MediumLayer& layer = scene.medium.layers[ev.layer];
        const Vec3f p = origin + dir * ev.t;
        const float uSelect = sampler.next1D();
        const Vec2f uLight = sampler.next2D();
        LightSample ls;
        if (sampleLight(scene, p, uSelect, uLight, ls)) {
          const float phase = phaseValue(layer, dir, ls.wi);
          const Vec3f tr = detail::shadowTransmittance(scene, tracer, p, ls);
          if (phase > 0 && !isBlack(tr)) {
            const float w = ls.delta ? 1.0f : powerHeuristic(ls.pdf, phase);
            L += beta * tr * ls.L * (phase * w / ls.pdf);
          }
        }
        float phasePdf = 0;
        const Vec3f wi = samplePhase(layer, dir, sampler.next2D(), phasePdf);  // weight phase / pdf = 1
        specular = false;
        prevPdf = phasePdf;
        prevP = p;
        if (depth + 1 > params.max_bounces) lastBounce = true;
        const float uRR = sampler.next1D();
        if (params.russian_roulette && depth >= 3 && !lastBounce) {
          const float q = std::fmax(0.05f, 1 - maxComponent(beta));
          if (uRR < q) break;
          beta = beta / (1 - q);
        }
        origin = p;
        dir = wi;
        continue;
      }
    }

    if (!hit.valid) {
      if (!scene.environment.isBlack()) {
        const Vec3f Le = environmentRadiance(scene.environment, dir);
        const float w = specular ? 1.0f : powerHeuristic(prevPdf, environmentPdf(scene, dir));
        L += beta * Le * w;
      }
      break;
    }

    const Vec3f wo = -dir;
    SurfaceInteraction si = makeSurfaceInteraction(scene, hit, wo);
    if (!isBlack(si.emission) && (si.front_face || si.double_sided)) {
      const float w = specular ? 1.0f : powerHeuristic(prevPdf, emissiveHitPdf(scene, hit, prevP, si));
      L += beta * si.emission * w;
    }
    if (lastBounce) break;

    detail::orientForShading(si, wo);
    const Vec3f woLocal = si.frame.toLocal(wo);
    const PrincipledBsdf bsdf(si.bsdf, woLocal);
    sampler.setDimension(kCameraDims + depth * kDimsPerBounce);

    // Next-event estimation.
    const float uSelect = sampler.next1D();
    const Vec2f uLight = sampler.next2D();
    if (!bsdf.isDelta()) {
      LightSample ls;
      if (sampleLight(scene, si.p, uSelect, uLight, ls)) {
        const Vec3f wiLocal = si.frame.toLocal(ls.wi);
        const float side = dot(ls.wi, si.ng);
        if (side * wiLocal.z > 0) {  // shading and geometric normals agree on the side
          float bsdfPdf = 0;
          const Vec3f f = bsdf.eval(woLocal, wiLocal, bsdfPdf);
          if (!isBlack(f)) {
            const Vec3f o = offsetRayOrigin(si.p, side > 0 ? si.ng : -si.ng);
            const Vec3f tr = detail::shadowTransmittance(scene, tracer, o, ls);
            if (!isBlack(tr)) {
              const float w = ls.delta ? 1.0f : powerHeuristic(ls.pdf, bsdfPdf);
              Vec3f c = beta * f * ls.L * (w / ls.pdf);
              if (!scene.medium.empty()) c *= tr;
              L += c;
            }
          }
        }
      }
    }

    // Continue the path.
    const float uLobe = sampler.next1D();
    const Vec2f uBsdf = sampler.next2D();
    BsdfSample bs;
    if (!bsdf.sample(woLocal, uLobe, uBsdf, bs)) break;
    const Vec3f wi = si.frame.toWorld(bs.wi);
    const float side = dot(wi, si.ng);
    if (side * bs.wi.z <= 0) break;
    beta *= bs.weight;
    if (isBlack(beta) || !isFinite(beta)) break;
    specular = bs.delta;
    prevPdf = bs.pdf;
    prevP = si.p;

    uint32_t kindCount = 0, kindCap = 0;
    switch (bs.kind) {
      case LobeKind::Diffuse: kindCount = ++nDiffuse; kindCap = params.max_diffuse; break;
      case LobeKind::Glossy: kindCount = ++nGlossy; kindCap = params.max_glossy; break;
      case LobeKind::Transmission: kindCount = ++nTrans; kindCap = params.max_transmission; break;
    }
    // The next vertex still contributes emission (direct light for MIS), but scatters no further.
    if (depth + 1 > params.max_bounces || kindCount > kindCap) lastBounce = true;

    const float uRR = sampler.next1D();
    if (params.russian_roulette && depth >= 3 && !lastBounce) {
      const float q = std::fmax(0.05f, 1 - maxComponent(beta));
      if (uRR < q) break;
      beta = beta / (1 - q);
    }
    origin = offsetRayOrigin(si.p, side > 0 ? si.ng : -si.ng);
    dir = wi;
  }
  return L;
}

// Single-hit views: ao (white where unoccluded within the distance), debug:normal, debug:albedo, debug:depth.
template <class Tracer>
Vec3f traceDebug(const Scene& scene, const Tracer& tracer, const IntegratorParams& params, Vec3f origin, Vec3f dir,
                 Sampler& sampler) {
  const Hit hit = tracer.intersect(origin, dir, detail::kInf);
  if (!hit.valid) return {};
  const Vec3f wo = -dir;
  SurfaceInteraction si = makeSurfaceInteraction(scene, hit, wo);
  switch (params.mode) {
    case Integrator::Ao: {
      if (dot(wo, si.ng) < 0) {
        si.ng = -si.ng;
        si.ns = -si.ns;
      }
      sampler.setDimension(kCameraDims);
      const Frame f(si.ns);
      const Vec3f d = f.toWorld(sampleCosineHemisphere(sampler.next2D()));
      if (dot(d, si.ng) <= 0) return {};
      return tracer.occluded(offsetRayOrigin(si.p, si.ng), d, params.ao_distance) ? Vec3f(0) : Vec3f(1);
    }
    case Integrator::DebugNormal: {
      const Vec3f n = dot(wo, si.ns) < 0 ? -si.ns : si.ns;
      return params.raw_normals ? n : n * 0.5f + Vec3f(0.5f);
    }
    case Integrator::DebugAlbedo: return si.bsdf.base_color;
    case Integrator::DebugDepth: return Vec3f(hit.t);
    default: return {};
  }
}

template <class Tracer>
Vec3f traceSample(const Scene& scene, const Tracer& tracer, const IntegratorParams& params, Vec3f origin, Vec3f dir,
                  Sampler& sampler) {
  return params.mode == Integrator::Path ? tracePath(scene, tracer, params, origin, dir, sampler)
                                         : traceDebug(scene, tracer, params, origin, dir, sampler);
}

}  // namespace pen
