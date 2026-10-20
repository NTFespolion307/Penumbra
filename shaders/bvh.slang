// Software ray tracing for the CUDA backend (GPUs without ray tracing hardware, DECISIONS D-086): a binary
// top-level BVH over the scene instances (built in backends/gpu/gpu_bvh.cpp) and one compressed 8-wide BVH per
// mesh built by tinybvh. The CWBVH traversal is a port of tinybvh's kernels/traverse_cwbvh.cl (MIT; based on
// CUDA code by AlanWBFT, after Ylitie et al. 2017, "Efficient Incoherent Ray Traversal on GPUs Through Compressed
// Wide BVHs"); the node format must match tinybvh's BVH8_CWBVH::ConvertFrom at the pinned version.
#pragma once
#include "scene.slang"

StructuredBuffer<float4> g_bvh_nodes;    // every mesh's CWBVH nodes, 5 float4 per node
StructuredBuffer<float4> g_bvh_tris;     // every mesh's triangles, 3 float4 each: v2 - v0, v1 - v0, v0 (w: triangle id)
StructuredBuffer<Blas> g_blas;           // per mesh
StructuredBuffer<TlasNode> g_tlas_nodes; // node 0 is the root
StructuredBuffer<uint> g_tlas_index;     // scene instances, in leaf order
StructuredBuffer<uint2> g_tri_map;       // triangle id -> (primitive within the mesh, triangle within the primitive)

static const uint kBlasNone = 0;
static const uint kBlasCwbvh = 1;
static const uint kBlasList = 2;
static const uint kBlasAlpha = 0x100;  // kind flag: some triangles have alpha cutouts
static const uint kCwbvhStack = 48;
static const uint kTlasStack = 64;

// 1 / x without infinities: an axis-parallel ray (an orthographic camera, for one) has exact zero components.
// The sign follows the sign bit, so -0 gives a large negative value; traversal takes direction signs from this
// reciprocal everywhere, so the octant and the box sides always agree.
float3 safeRcp(float3 d) {
  float3 r;
  [unroll] for (int i = 0; i < 3; ++i) {
    float x = d[i];
    if (abs(x) < 1e-20) x = (asuint(x) & 0x80000000u) != 0 ? -1e-20 : 1e-20;
    r[i] = 1 / x;
  }
  return r;
}

// Index of the highest set bit of v (v != 0). Not firstbithigh alone: Slang's CUDA prelude (2026.19) gives
// uint values with bit 31 set the signed meaning (it inverts them first), and CWBVH hit masks live in bits 24..31.
uint highestBit(uint v) { return (v & 0x80000000u) != 0 ? 31u : firstbithigh(v); }

bool hitBox(float3 lo, float3 hi, float3 o, float3 rd, float tmax) {
  float3 t0 = (lo - o) * rd, t1 = (hi - o) * rd;
  float3 tn3 = min(t0, t1), tf3 = max(t0, t1);
  float tn = max(max(tn3.x, tn3.y), max(tn3.z, 0.0));
  float tf = min(min(tf3.x, tf3.y), min(tf3.z, tmax));
  return tn <= tf;
}

// 1 + q / 256 for the four quantized bytes of w (tinybvh's unpack_q8x4).
float4 unpackQ8x4(uint w) {
  const uint m = 0x007F8000u, b = 0x3F800000u;
  return float4(asfloat(((w << 15) & m) | b), asfloat(((w << 7) & m) | b), asfloat(((w >> 1) & m) | b),
                asfloat(((w >> 9) & m) | b));
}

// Hit bits of four children of a CWBVH node (one meta word; lo/hi already swapped for negative directions).
uint childHits(uint meta4, uint octinv4, uint qlox, uint qhix, uint qloy, uint qhiy, uint qloz, uint qhiz, float3 idir,
               float3 orig, float tmax) {
  uint isInner4 = (meta4 & (meta4 << 1)) & 0x10101010u;
  uint innerMask4 = (isInner4 << 4) - (isInner4 >> 4);
  uint bitIndex4 = (meta4 ^ (octinv4 & innerMask4)) & 0x1F1F1F1Fu;
  uint childBits4 = (meta4 >> 5) & 0x07070707u;
  float4 tminx = unpackQ8x4(qlox) * idir.x + orig.x, tmaxx = unpackQ8x4(qhix) * idir.x + orig.x;
  float4 tminy = unpackQ8x4(qloy) * idir.y + orig.y, tmaxy = unpackQ8x4(qhiy) * idir.y + orig.y;
  float4 tminz = unpackQ8x4(qloz) * idir.z + orig.z, tmaxz = unpackQ8x4(qhiz) * idir.z + orig.z;
  float4 tn = max(max(tminx, tminy), max(tminz, float4(0, 0, 0, 0)));
  float4 tf = min(min(tmaxx, tmaxy), min(tmaxz, float4(tmax, tmax, tmax, tmax)));
  uint hm = 0;
  if (tn.x <= tf.x) hm |= (childBits4 & 255u) << (bitIndex4 & 31u);
  if (tn.y <= tf.y) hm |= ((childBits4 >> 8) & 255u) << ((bitIndex4 >> 8) & 31u);
  if (tn.z <= tf.z) hm |= ((childBits4 >> 16) & 255u) << ((bitIndex4 >> 16) & 31u);
  if (tn.w <= tf.w) hm |= (childBits4 >> 24) << (bitIndex4 >> 24);
  return hm;
}

struct BlasHit {
  float t;
  float u;
  float v;
  uint tri;  // triangle id (g_tri_map)
  bool found;
};

// Moller-Trumbore on one stored triangle; updates h when it is nearer than h.t and survives the alpha cutout.
// Written so NaNs (degenerate triangles, parallel rays) never count as hits.
bool testTriangle(uint addr, uint instance, bool alpha, float3 O, float3 D, inout BlasHit h) {
  float4 a = g_bvh_tris[addr], b = g_bvh_tris[addr + 1], v0 = g_bvh_tris[addr + 2];
  float3 e1 = a.xyz, e2 = b.xyz;
  float3 r = cross(D, e1);
  float f = 1 / dot(e2, r);
  float3 s = O - v0.xyz;
  float u = f * dot(s, r);
  float3 q = cross(s, e2);
  float v = f * dot(D, q);
  if (!(u >= 0 && v >= 0 && u + v <= 1)) return false;
  float t = f * dot(e1, q);
  if (!(t > 0 && t < h.t)) return false;
  uint tri = asuint(v0.w);
  if (alpha) {
    uint2 pt = g_tri_map[tri];
    if (alphaRejects(instance, pt.x, pt.y, u, v)) return false;
  }
  h.t = t;
  h.u = u;
  h.v = v;
  h.tri = tri;
  h.found = true;
  return true;
}

// Closest hit (anyHit false) or first hit (anyHit true) in one mesh, in object space; h.t is the current limit.
void traverseBlas(Blas blas, uint instance, float3 O, float3 D, bool anyHit, inout BlasHit h,
                  inout uint steps) {
  uint kind = blas.kind & 0xffu;
  bool alpha = (blas.kind & kBlasAlpha) != 0;
  if (kind == kBlasList) {
    for (uint i = 0; i < blas.tri_count; ++i) {
      ++steps;
      if (testTriangle(blas.tri_base + i * 3, instance, alpha, O, D, h) && anyHit) return;
    }
    return;
  }
  if (kind != kBlasCwbvh) return;

  float3 rD = safeRcp(D);
  uint2 stack[kCwbvhStack];
  uint sp = 0;
  uint octinv4 = (7u - ((rD.x < 0 ? 4u : 0u) | (rD.y < 0 ? 2u : 0u) | (rD.z < 0 ? 1u : 0u))) * 0x1010101u;
  float3 rDs = rD * 256.0;
  uint2 ngroup = uint2(0, 0x80000000u), tgroup = uint2(0, 0);
  for (;;) {
    if (ngroup.y > 0x00FFFFFFu) {
      uint hits = ngroup.y, imask = ngroup.y;
      uint childBit = highestBit(hits);
      uint childBase = ngroup.x;
      ngroup.y &= ~(1u << childBit);
      if (ngroup.y > 0x00FFFFFFu) {
        if (sp < kCwbvhStack) {
          stack[sp++] = ngroup;
        } else {
          InterlockedAdd(g_counters[1], 1);  // reported by the backend; never happens on sane trees
        }
      }
      uint slot = (childBit - 24) ^ (octinv4 & 255u);
      uint rel = countbits(imask & ~(0xFFFFFFFFu << slot));
      uint ci = blas.node_base + (childBase + rel) * 5;
      float4 n0 = g_bvh_nodes[ci], n1 = g_bvh_nodes[ci + 1], n2 = g_bvh_nodes[ci + 2];
      float4 n3 = g_bvh_nodes[ci + 3], n4 = g_bvh_nodes[ci + 4];
      ++steps;
      uint e4 = asuint(n0.w);
      ngroup.x = asuint(n1.x);
      tgroup = uint2(asuint(n1.y), 0);
      float3 idir = float3(asfloat((e4 & 255u) << 23) * rDs.x, asfloat(((e4 >> 8) & 255u) << 23) * rDs.y,
                           asfloat(((e4 >> 16) & 255u) << 23) * rDs.z);
      float3 orig = (n0.xyz - O) * rD - idir;
      uint hitmask = childHits(asuint(n1.z), octinv4, asuint(rD.x < 0 ? n3.z : n2.x), asuint(rD.x < 0 ? n2.x : n3.z),
                               asuint(rD.y < 0 ? n4.x : n2.z), asuint(rD.y < 0 ? n2.z : n4.x),
                               asuint(rD.z < 0 ? n4.z : n3.x), asuint(rD.z < 0 ? n3.x : n4.z), idir, orig, h.t);
      hitmask |= childHits(asuint(n1.w), octinv4, asuint(rD.x < 0 ? n3.w : n2.y), asuint(rD.x < 0 ? n2.y : n3.w),
                           asuint(rD.y < 0 ? n4.y : n2.w), asuint(rD.y < 0 ? n2.w : n4.y),
                           asuint(rD.z < 0 ? n4.w : n3.y), asuint(rD.z < 0 ? n3.y : n4.w), idir, orig, h.t);
      ngroup.y = (hitmask & 0xFF000000u) | (e4 >> 24);
      tgroup.y = hitmask & 0x00FFFFFFu;
    } else {
      tgroup = ngroup;
      ngroup = uint2(0, 0);
    }
    while (tgroup.y != 0) {
      uint ti = highestBit(tgroup.y);
      tgroup.y -= 1u << ti;
      ++steps;
      if (testTriangle(blas.tri_base + tgroup.x + ti * 3, instance, alpha, O, D, h) && anyHit) return;
    }
    if (ngroup.y <= 0x00FFFFFFu) {
      if (sp == 0) return;
      ngroup = stack[--sp];
    }
  }
}

// The whole scene: TLAS, then each candidate instance's mesh in object space (the direction is not normalized
// there, so t stays the world-space ray parameter).
BlasHit traceScene(float3 o, float3 d, float tmax, bool anyHit, out uint instanceHit, inout uint steps) {
  BlasHit h;
  h.t = tmax;
  h.u = 0;
  h.v = 0;
  h.tri = 0;
  h.found = false;
  instanceHit = 0;
  uint instances, stride;
  g_tlas_index.GetDimensions(instances, stride);
  if (instances == 0) return h;  // nothing to hit (the TLAS is a placeholder leaf)
  float3 rd = safeRcp(d);
  uint stack[kTlasStack];
  uint sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    TlasNode n = g_tlas_nodes[stack[--sp]];
    ++steps;
    if (!hitBox(n.lo, n.hi, o, rd, h.t)) continue;
    if (n.count == 0) {
      if (sp + 2 <= kTlasStack) {
        stack[sp++] = n.left_first + 1;
        stack[sp++] = n.left_first;
      } else {
        InterlockedAdd(g_counters[1], 1);
      }
      continue;
    }
    for (uint k = 0; k < n.count; ++k) {
      uint i = g_tlas_index[n.left_first + k];
      Instance inst = g_instances[i];
      float3 oi = xformPoint(inst.inverse, o), di = xformVector(inst.inverse, d);
      bool before = h.found;
      h.found = false;
      traverseBlas(g_blas[inst.mesh], i, oi, di, anyHit, h, steps);
      if (h.found) {
        instanceHit = i;
        if (anyHit) return h;
      }
      h.found = h.found || before;
    }
  }
  return h;
}

Hit intersect(float3 o, float3 d) {
  uint steps = 0, instance;
  BlasHit b = traceScene(o, d, kInfinity, false, instance, steps);
  Hit h;
  h.valid = b.found;
  h.t = b.t;
  h.instance = instance;
  uint2 pt = b.found ? g_tri_map[b.tri] : uint2(0, 0);
  h.primitive = pt.x;
  h.triangle = pt.y;
  h.u = b.u;
  h.v = b.v;
  h.steps = steps;
  return h;
}

bool occluded(float3 o, float3 d, float tmax) {
  uint steps = 0, instance;
  return traceScene(o, d, tmax, true, instance, steps).found;
}
