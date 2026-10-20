// The GPU path tracer, shared by both GPU backends: port of core/render/integrator.h. Each thread runs the whole
// path for one pixel and every sample of the launch (megakernel, DECISIONS D-078).
//
// The including file declares how rays are traced before including this one (DECISIONS D-085):
//   Hit intersect(float3 o, float3 d);              closest hit, alpha cutouts applied
//   bool occluded(float3 o, float3 d, float tmax);  any hit in (0, tmax), alpha cutouts applied
// pathtrace.slang implements them with Vulkan ray tracing, pathtrace_cuda.slang with the software BVH (bvh.slang).
#pragma once
#include "lights.slang"
#include "camera.slang"
#include "sampler.slang"

[[vk::binding(23, 0)]] RWStructuredBuffer<float3> g_out;  // the work item's tile, row-major

// PathSampler dimension layout, as core/render/integrator.h.
static const uint kCameraDims = 2;
static const uint kDimsPerBounce = 5;

void orientForShading(inout SurfaceInteraction si, float3 wo) {
  bool transmissive = si.bsdf.transmission > 0 && si.bsdf.metallic < 1;
  if (!transmissive && dot(wo, si.ng) < 0) {
    si.ng = -si.ng;
    si.ns = -si.ns;
    si.frame = frameFromNormalTangent(si.ns, si.frame.t);
  }
  if (dot(wo, si.ns) * dot(wo, si.ng) <= 0) {
    si.ns = si.ng;
    si.frame = frameFromNormalTangent(si.ns, si.frame.t);
  }
}

float3 tracePath(float3 origin, float3 dir, inout PathSampler sampler) {
  FrameData f = g_frame;
  float3 L = float3(0, 0, 0), beta = float3(1, 1, 1);
  bool specular = true;
  float prevPdf = 0;
  float3 prevP = origin;
  uint nDiffuse = 0, nGlossy = 0, nTrans = 0;
  bool lastBounce = false;

  for (uint depth = 0;; ++depth) {
    Hit hit = intersect(origin, dir);
    if (!hit.valid) {
      if (f.env_is_black == 0) {
        float3 Le = environmentRadiance(dir);
        float w = specular ? 1.0 : powerHeuristic(prevPdf, environmentPdf(dir));
        L += beta * Le * w;
      }
      break;
    }

    float3 wo = -dir;
    SurfaceInteraction si = makeSurfaceInteraction(hit, wo);
    if (!isBlack(si.emission) && (si.front_face || si.double_sided)) {
      float w = specular ? 1.0 : powerHeuristic(prevPdf, emissiveHitPdf(hit, prevP, si));
      L += beta * si.emission * w;
    }
    if (lastBounce) break;

    orientForShading(si, wo);
    float3 woLocal = si.frame.toLocal(wo);
    PrincipledBsdf bsdf = makeBsdf(si.bsdf, woLocal);
    sampler.setDimension(kCameraDims + depth * kDimsPerBounce);

    // Next-event estimation.
    float uSelect = sampler.next1D();
    float2 uLight = sampler.next2D();
    if (!bsdf.isDelta()) {
      LightSample ls;
      if (sampleLight(si.p, uSelect, uLight, ls)) {
        float3 wiLocal = si.frame.toLocal(ls.wi);
        float side = dot(ls.wi, si.ng);
        if (side * wiLocal.z > 0) {
          float bsdfPdf = 0;
          float3 fv = bsdf.eval(woLocal, wiLocal, bsdfPdf);
          if (!isBlack(fv)) {
            float3 o = offsetRayOrigin(si.p, side > 0 ? si.ng : -si.ng);
            bool blocked;
            if (isinf(ls.dist)) {
              blocked = occluded(o, ls.wi, kInfinity);
            } else {
              float3 target = offsetRayOrigin(ls.p_light, dot(ls.n_light, -ls.wi) >= 0 ? ls.n_light : -ls.n_light);
              float3 toTarget = (ls.delta ? ls.p_light : target) - o;
              float d = length(toTarget);
              blocked = occluded(o, toTarget / d, d * (1 - 1e-4));
            }
            if (!blocked) {
              float w = ls.delta ? 1.0 : powerHeuristic(ls.pdf, bsdfPdf);
              L += beta * fv * ls.L * (w / ls.pdf);
            }
          }
        }
      }
    }

    // Continue the path.
    float uLobe = sampler.next1D();
    float2 uBsdf = sampler.next2D();
    BsdfSample bs;
    if (!bsdf.sample(woLocal, uLobe, uBsdf, bs)) break;
    float3 wi = si.frame.toWorld(bs.wi);
    float side = dot(wi, si.ng);
    if (side * bs.wi.z <= 0) break;
    beta *= bs.weight;
    if (isBlack(beta) || !isFinite3(beta)) break;
    specular = bs.delta;
    prevPdf = bs.pdf;
    prevP = si.p;

    uint kindCount = 0, kindCap = 0;
    if (bs.kind == kLobeDiffuse) {
      kindCount = ++nDiffuse;
      kindCap = f.max_diffuse;
    } else if (bs.kind == kLobeGlossy) {
      kindCount = ++nGlossy;
      kindCap = f.max_glossy;
    } else {
      kindCount = ++nTrans;
      kindCap = f.max_transmission;
    }
    if (depth + 1 > f.max_bounces || kindCount > kindCap) lastBounce = true;

    float uRR = sampler.next1D();
    if (f.russian_roulette != 0 && depth >= 3 && !lastBounce) {
      float q = max(0.05, 1 - maxComponent(beta));
      if (uRR < q) break;
      beta = beta / (1 - q);
    }
    origin = offsetRayOrigin(si.p, side > 0 ? si.ng : -si.ng);
    dir = wi;
  }
  return L;
}

float3 traceDebug(float3 origin, float3 dir, inout PathSampler sampler) {
  Hit hit = intersect(origin, dir);
  uint mode = g_frame.mode;
  if (mode == kModeDebugBvh) return float3(hit.steps, hit.steps, hit.steps);
  if (!hit.valid) return float3(0, 0, 0);
  float3 wo = -dir;
  SurfaceInteraction si = makeSurfaceInteraction(hit, wo);
  if (mode == kModeAo) {
    if (dot(wo, si.ng) < 0) {
      si.ng = -si.ng;
      si.ns = -si.ns;
    }
    sampler.setDimension(kCameraDims);
    Frame fr = frameFromNormal(si.ns);
    float3 d = fr.toWorld(sampleCosineHemisphere(sampler.next2D()));
    if (dot(d, si.ng) <= 0) return float3(0, 0, 0);
    return occluded(offsetRayOrigin(si.p, si.ng), d, g_frame.ao_distance) ? float3(0, 0, 0) : float3(1, 1, 1);
  }
  if (mode == kModeDebugNormal) {
    float3 n = dot(wo, si.ns) < 0 ? -si.ns : si.ns;
    return n * 0.5 + float3(0.5, 0.5, 0.5);
  }
  if (mode == kModeDebugAlbedo) return si.bsdf.base_color;
  if (mode == kModeDebugDepth) return float3(hit.t, hit.t, hit.t);
  return float3(0, 0, 0);
}

// One pixel of a launch: samples [item.sample_begin, item.sample_end), summed in order on top of the sum the
// previous launch left (kItemAccumulate); the last launch writes the mean (kItemFinish).
void renderPixel(uint2 local, PushConstants item) {
  if (local.x >= item.width || local.y >= item.height) return;
  uint x = item.x0 + local.x, y = item.y0 + local.y;
  uint o = local.y * item.width + local.x;
  float3 sum = (item.flags & kItemAccumulate) != 0 ? g_out[o] : float3(0, 0, 0);
  uint nans = 0;
  for (uint s = item.sample_begin; s < item.sample_end; ++s) {
    PathSampler sampler = makeSampler(x, y, s, g_frame.seed);
    float2 jitter = sampler.next2D();
    float2 lens = sampler.next2D();
    float3 ro, rd;
    generateRay(x + jitter.x, y + jitter.y, lens, ro, rd);
    float3 L = g_frame.mode == kModePath ? tracePath(ro, rd, sampler) : traceDebug(ro, rd, sampler);
    if (!isFinite3(L)) {
      ++nans;
      L = float3(0, 0, 0);
    }
    sum += L;
  }
  if ((item.flags & kItemFinish) != 0) {
    float invN = item.sample_count > 0 ? 1.0 / float(item.sample_count) : 0.0;
    g_out[o] = sum * invN;
  } else {
    g_out[o] = sum;
  }
  if (nans > 0) InterlockedAdd(g_counters[0], nans);
}
