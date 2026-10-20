// The Vulkan RT path tracer: integrator.slang with rays traced by the hardware (TraceRay). The ray generation
// shader runs the whole path for every pixel and sample of a work item (megakernel, DECISIONS D-078); the hit
// shaders only report the hit and apply alpha cutouts.
#include "scene.slang"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
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
  h.steps = 0;
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

#include "integrator.slang"

[shader("raygeneration")]
void raygen() { renderPixel(DispatchRaysIndex().xy, g_item); }

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
