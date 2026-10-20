// The Vulkan RT path tracer: port of core/render/integrator.h. The ray generation shader runs the whole path
// for every pixel and sample of a work item (megakernel, DECISIONS D-078); the hit shaders only report the hit
// and apply alpha cutouts.
#include "lights.slang"
#include "camera.slang"
#include "sampler.slang"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::binding(23, 0)]] RWStructuredBuffer<float3> g_out;    // the work item's tile, row-major
[[vk::binding(24, 0)]] RWStructuredBuffer<uint> g_counters;  // [0] non-finite samples
[[vk::push_constant]] ConstantBuffer<PushConstants> g_item;

struct Payload {
  uint hit;
  float t;
  uint instance;
  uint primitive;
  uint triangle;
  float u;
  float v;
};

// PathSampler dimension layout, as core/render/integrator.h.
static const uint kCameraDims = 2;
static const uint kDimsPerBounce = 5;

Hit intersect(float3 o, float3 d) {
  RayDesc ray;
  ray.Origin = o;
  ray.Direction = d;
  ray.TMin = 0;
  ray.TMax = kInfinity;
  Payload p;
  p.hit = 0;
  TraceRay(g_tlas, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, p);
  Hit h;
  h.valid = p.hit != 0;
  h.t = p.t;
  h.instance = p.instance;
  h.primitive = p.primitive;
  h.triangle = p.triangle;
  h.u = p.u;
  h.v = p.v;
  return h;
}

bool occluded(float3 o, float3 d, float tmax) {
  RayDesc ray;
  ray.Origin = o;
  ray.Direction = d;
  ray.TMin = 0;
  ray.TMax = tmax;
  Payload p;
  p.hit = 1;  // the miss shader clears it
  TraceRay(g_tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff, 0, 0, 0, ray, p);
  return p.hit != 0;
}

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
  if (!hit.valid) return float3(0, 0, 0);
  float3 wo = -dir;
  SurfaceInteraction si = makeSurfaceInteraction(hit, wo);
  uint mode = g_frame.mode;
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

[shader("raygeneration")]
void raygen() {
  uint2 local = DispatchRaysIndex().xy;
  if (local.x >= g_item.width || local.y >= g_item.height) return;
  uint x = g_item.x0 + local.x, y = g_item.y0 + local.y;
  float3 sum = float3(0, 0, 0);
  uint nans = 0;
  for (uint s = g_item.sample_begin; s < g_item.sample_end; ++s) {
    PathSampler sampler = makeSampler(x, y, s, g_frame.seed);
    float2 jitter = sampler.next2D();
    float2 lens = sampler.next2D();
    float3 o, d;
    generateRay(x + jitter.x, y + jitter.y, lens, o, d);
    float3 L = g_frame.mode == kModePath ? tracePath(o, d, sampler) : traceDebug(o, d, sampler);
    if (!isFinite3(L)) {
      ++nans;
      L = float3(0, 0, 0);
    }
    sum += L;
  }
  uint n = g_item.sample_end - g_item.sample_begin;
  float invN = n > 0 ? 1.0 / float(n) : 0.0;
  g_out[local.y * g_item.width + local.x] = sum * invN;
  if (nans > 0) InterlockedAdd(g_counters[0], nans);
}

[shader("closesthit")]
void closestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
  p.hit = 1;
  p.t = RayTCurrent();
  p.instance = InstanceID();  // custom index = scene instance (empty meshes are left out of the TLAS)
  p.primitive = GeometryIndex();
  p.triangle = PrimitiveIndex();
  p.u = attr.barycentrics.x;
  p.v = attr.barycentrics.y;
}

[shader("anyhit")]
void anyHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
  if (alphaRejects(InstanceID(), GeometryIndex(), PrimitiveIndex(), attr.barycentrics.x, attr.barycentrics.y))
    IgnoreHit();
}

[shader("miss")]
void miss(inout Payload p) { p.hit = 0; }
