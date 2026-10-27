// Scene buffers and the port of core/render/surface.cpp (surface interaction, alpha, ray offsets) and of
// Texture::sample in core/image/image.cpp.
#pragma once
#include "types.slang"
#include "bsdf.slang"

[[vk::binding(1, 0)]] ConstantBuffer<FrameData> g_frame;
[[vk::binding(2, 0)]] StructuredBuffer<Instance> g_instances;
[[vk::binding(3, 0)]] StructuredBuffer<Primitive> g_primitives;
[[vk::binding(4, 0)]] StructuredBuffer<float3> g_positions;
[[vk::binding(5, 0)]] StructuredBuffer<float3> g_normals;
[[vk::binding(6, 0)]] StructuredBuffer<float4> g_tangents;
[[vk::binding(7, 0)]] StructuredBuffer<float2> g_uvs;
[[vk::binding(8, 0)]] StructuredBuffer<float4> g_colors;
[[vk::binding(9, 0)]] StructuredBuffer<uint> g_indices;
[[vk::binding(10, 0)]] StructuredBuffer<Material> g_materials;
[[vk::binding(11, 0)]] StructuredBuffer<Texture> g_textures;
// Texels in up to four parts of at most 2 GB each: one storage buffer may span at most 4 GB on common drivers
// (maxStorageBufferRange), and textures can exceed that (D-109). A texture lies in one part (Texture.part).
[[vk::binding(12, 0)]] StructuredBuffer<uint> g_texels8;     // RGBA8, R in the low byte
[[vk::binding(25, 0)]] StructuredBuffer<uint> g_texels8_1;
[[vk::binding(26, 0)]] StructuredBuffer<uint> g_texels8_2;
[[vk::binding(27, 0)]] StructuredBuffer<uint> g_texels8_3;
[[vk::binding(13, 0)]] StructuredBuffer<float4> g_texels32;
[[vk::binding(28, 0)]] StructuredBuffer<float4> g_texels32_1;
[[vk::binding(29, 0)]] StructuredBuffer<float4> g_texels32_2;
[[vk::binding(30, 0)]] StructuredBuffer<float4> g_texels32_3;
[[vk::binding(21, 0)]] StructuredBuffer<float> g_srgb_lut;   // 256 entries, as the CPU's table
// [0] non-finite samples, [1] BVH traversals cut short by a full stack (CUDA); read back after every item.
[[vk::binding(24, 0)]] RWStructuredBuffer<uint> g_counters;

struct Hit {
  bool valid;
  float t;
  uint instance;
  uint primitive;
  uint triangle;
  float u;
  float v;
  uint steps;  // BVH nodes visited plus triangles tested (software traversal only; debug:bvh)
};

struct SurfaceInteraction {
  float3 p;
  float3 ng;
  float3 ns;
  Frame frame;
  float2 uv0;
  float2 uv1;
  uint material;
  BsdfInputs bsdf;
  float3 emission;
  float alpha;
  bool double_sided;
  bool front_face;
};

// ---- Textures ----

int wrapCoord(int i, int n, uint mode) {
  if (mode == 1) return i < 0 ? 0 : (i >= n ? n - 1 : i);
  if (mode == 2) {
    int period = 2 * n;
    int m = i % period;
    if (m < 0) m += period;
    return m < n ? m : period - 1 - m;
  }
  int m = i % n;
  return m < 0 ? m + n : m;
}

float4 texel(Texture tex, int x, int y, bool srgb) {
  x = wrapCoord(x, int(tex.width), tex.wrap_s);
  y = wrapCoord(y, int(tex.height), tex.wrap_t);
  uint i = uint(y) * tex.width + uint(x);
  i += tex.offset;
  if (tex.is_float != 0) {
    switch (tex.part) {
      case 1: return g_texels32_1[i];
      case 2: return g_texels32_2[i];
      case 3: return g_texels32_3[i];
      default: return g_texels32[i];
    }
  }
  uint p;
  switch (tex.part) {
    case 1: p = g_texels8_1[i]; break;
    case 2: p = g_texels8_2[i]; break;
    case 3: p = g_texels8_3[i]; break;
    default: p = g_texels8[i]; break;
  }
  uint r = p & 0xffu, g = (p >> 8) & 0xffu, b = (p >> 16) & 0xffu, a = p >> 24;
  if (srgb) return float4(g_srgb_lut[r], g_srgb_lut[g], g_srgb_lut[b], a / 255.0);
  return float4(r / 255.0, g / 255.0, b / 255.0, a / 255.0);
}

float4 sampleTexture(Texture tex, float2 uv, bool srgb) {
  if (tex.width == 0 || tex.height == 0) return float4(1, 1, 1, 1);
  float fx = uv.x * tex.width - 0.5, fy = uv.y * tex.height - 0.5;
  if (tex.nearest != 0) return texel(tex, int(floor(fx + 0.5)), int(floor(fy + 0.5)), srgb);
  float x0f = floor(fx), y0f = floor(fy);
  int x0 = int(x0f), y0 = int(y0f);
  float tx = fx - x0f, ty = fy - y0f;
  float4 a = texel(tex, x0, y0, srgb), b = texel(tex, x0 + 1, y0, srgb);
  float4 c = texel(tex, x0, y0 + 1, srgb), d = texel(tex, x0 + 1, y0 + 1, srgb);
  float4 ab = a + (b - a) * tx, cd = c + (d - c) * tx;
  return ab + (cd - ab) * ty;
}

float2 applyTransform(TextureRef r, float2 uv) {
  if (r.has_transform == 0) return uv;
  float c = cos(r.rotation), s = sin(r.rotation);
  float2 sc = float2(uv.x * r.scale.x, uv.y * r.scale.y);
  return float2(c * sc.x + s * sc.y + r.offset.x, -s * sc.x + c * sc.y + r.offset.y);
}

float4 lookup(TextureRef r, float2 uv0, float2 uv1, bool srgb) {
  if (r.index < 0) return float4(1, 1, 1, 1);
  float2 uv = applyTransform(r, r.tex_coord == 1 ? uv1 : uv0);
  return sampleTexture(g_textures[r.index], uv, srgb);
}

// ---- Geometry ----

float3 offsetRayOrigin(float3 p, float3 n) {
  const float kOrigin = 1.0 / 32.0, kFloatScale = 1.0 / 65536.0, kIntScale = 256.0;
  float3 o;
  [unroll] for (int i = 0; i < 3; ++i) {
    int off = int(kIntScale * n[i]);
    int bits = asint(p[i]) + (p[i] < 0 ? -off : off);
    o[i] = abs(p[i]) < kOrigin ? p[i] + kFloatScale * n[i] : asfloat(bits);
  }
  return o;
}

float alphaAt(uint instance, uint primitive, uint triangle, float u, float v) {
  Instance inst = g_instances[instance];
  Primitive prim = g_primitives[inst.primitive_base + primitive];
  Material mat = g_materials[prim.material];
  float alpha = mat.base_color.w;
  uint i0 = g_indices[prim.index_offset + triangle * 3], i1 = g_indices[prim.index_offset + triangle * 3 + 1],
       i2 = g_indices[prim.index_offset + triangle * 3 + 2];
  float b0 = 1 - u - v;
  if (prim.color_offset >= 0) {
    float4 c0 = g_colors[prim.color_offset + i0], c1 = g_colors[prim.color_offset + i1], c2 = g_colors[prim.color_offset + i2];
    alpha *= c0.w * b0 + c1.w * u + c2.w * v;
  }
  if (mat.base_color_tex.index >= 0) {
    float2 uv0 = float2(0, 0), uv1 = float2(0, 0);
    if (prim.uv0_offset >= 0)
      uv0 = g_uvs[prim.uv0_offset + i0] * b0 + g_uvs[prim.uv0_offset + i1] * u + g_uvs[prim.uv0_offset + i2] * v;
    if (prim.uv1_offset >= 0)
      uv1 = g_uvs[prim.uv1_offset + i0] * b0 + g_uvs[prim.uv1_offset + i1] * u + g_uvs[prim.uv1_offset + i2] * v;
    alpha *= lookup(mat.base_color_tex, uv0, uv1, true).w;
  }
  return alpha;
}

// True when the hit should be ignored (alpha cutout; BLEND is a 0.5 cutout, DECISIONS D-024).
bool alphaRejects(uint instance, uint primitive, uint triangle, float u, float v) {
  Instance inst = g_instances[instance];
  Material mat = g_materials[g_primitives[inst.primitive_base + primitive].material];
  if (mat.alpha_mode == 0) return false;
  float cutoff = mat.alpha_mode == 1 ? mat.alpha_cutoff : 0.5;
  return alphaAt(instance, primitive, triangle, u, v) < cutoff;
}

SurfaceInteraction makeSurfaceInteraction(Hit hit, float3 wo) {
  Instance inst = g_instances[hit.instance];
  Primitive prim = g_primitives[inst.primitive_base + hit.primitive];
  Material mat = g_materials[prim.material];
  uint i0 = g_indices[prim.index_offset + hit.triangle * 3], i1 = g_indices[prim.index_offset + hit.triangle * 3 + 1],
       i2 = g_indices[prim.index_offset + hit.triangle * 3 + 2];
  float b1 = hit.u, b2 = hit.v, b0 = 1 - b1 - b2;

  SurfaceInteraction si;
  si.material = prim.material;
  si.double_sided = mat.double_sided != 0;
  float3 p0 = g_positions[prim.position_offset + i0], p1 = g_positions[prim.position_offset + i1],
         p2 = g_positions[prim.position_offset + i2];
  si.p = xformPoint(inst.to_world, p0 * b0 + p1 * b1 + p2 * b2);
  si.ng = normalizeSafe(xformNormal(inst.inverse, cross(p1 - p0, p2 - p0)));
  if (prim.normal_offset >= 0) {
    float3 n = g_normals[prim.normal_offset + i0] * b0 + g_normals[prim.normal_offset + i1] * b1 +
               g_normals[prim.normal_offset + i2] * b2;
    si.ns = normalizeSafe(xformNormal(inst.inverse, n));
  } else {
    si.ns = si.ng;
  }
  if (dot(si.ns, si.ng) < 0) si.ns = -si.ns;
  si.uv0 = float2(0, 0);
  si.uv1 = float2(0, 0);
  if (prim.uv0_offset >= 0)
    si.uv0 = g_uvs[prim.uv0_offset + i0] * b0 + g_uvs[prim.uv0_offset + i1] * b1 + g_uvs[prim.uv0_offset + i2] * b2;
  if (prim.uv1_offset >= 0)
    si.uv1 = g_uvs[prim.uv1_offset + i0] * b0 + g_uvs[prim.uv1_offset + i1] * b1 + g_uvs[prim.uv1_offset + i2] * b2;

  float3 tangent = float3(0, 0, 0);
  float handed = 1;
  if (prim.tangent_offset >= 0) {
    float4 t = g_tangents[prim.tangent_offset + i0] * b0 + g_tangents[prim.tangent_offset + i1] * b1 +
               g_tangents[prim.tangent_offset + i2] * b2;
    tangent = xformVector(inst.to_world, t.xyz);
    handed = t.w < 0 ? -1.0 : 1.0;
  }

  // ---- Material inputs ----
  float4 base = mat.base_color;
  float4 bt = lookup(mat.base_color_tex, si.uv0, si.uv1, true);
  base = base * bt;
  if (prim.color_offset >= 0) {
    float4 c = g_colors[prim.color_offset + i0] * b0 + g_colors[prim.color_offset + i1] * b1 +
               g_colors[prim.color_offset + i2] * b2;
    base = base * c;
  }
  si.bsdf.base_color = base.xyz;
  si.alpha = base.w;
  float4 mr = lookup(mat.metallic_roughness_tex, si.uv0, si.uv1, false);
  si.bsdf.metallic = mat.metallic * mr.z;
  si.bsdf.roughness = mat.roughness * mr.y;
  si.bsdf.transmission = mat.transmission * lookup(mat.transmission_tex, si.uv0, si.uv1, false).x;
  si.bsdf.ior = mat.ior;
  si.bsdf.specular = mat.specular * lookup(mat.specular_tex, si.uv0, si.uv1, false).w;
  si.bsdf.specular_color = mat.specular_color * lookup(mat.specular_color_tex, si.uv0, si.uv1, true).xyz;
  si.bsdf.clearcoat = mat.clearcoat * lookup(mat.clearcoat_tex, si.uv0, si.uv1, false).x;
  si.bsdf.clearcoat_roughness = mat.clearcoat_roughness * lookup(mat.clearcoat_roughness_tex, si.uv0, si.uv1, false).y;
  float4 sc = lookup(mat.sheen_color_tex, si.uv0, si.uv1, true);
  si.bsdf.sheen_color = mat.sheen_color * sc.xyz;
  si.bsdf.sheen_roughness = mat.sheen_roughness * lookup(mat.sheen_roughness_tex, si.uv0, si.uv1, false).w;
  si.emission = float3(0, 0, 0);
  if (mat.emits != 0)
    si.emission = mat.emissive * lookup(mat.emissive_tex, si.uv0, si.uv1, true).xyz * mat.emissive_strength;

  // ---- Normal map ----
  bool hasTangent = lengthSquared(tangent) > 0;
  if (mat.normal_tex.index >= 0 && hasTangent) {
    float4 tn = lookup(mat.normal_tex, si.uv0, si.uv1, false);
    float nx = (tn.x * 2 - 1) * mat.normal_scale, ny = (tn.y * 2 - 1) * mat.normal_scale, nz = tn.z * 2 - 1;
    Frame tf = frameFromNormalTangent(si.ns, tangent);
    float3 bit = tf.b * handed;
    float3 n = normalizeSafe(tf.t * nx + bit * ny + tf.n * nz);
    if (dot(n, si.ng) > 0) si.ns = n;
  }

  // ---- Anisotropy direction ----
  float aniso = mat.anisotropy;
  float dirX = cos(mat.anisotropy_rotation), dirY = sin(mat.anisotropy_rotation);
  if (mat.anisotropy_tex.index >= 0) {
    float4 at = lookup(mat.anisotropy_tex, si.uv0, si.uv1, false);
    float tx = at.x * 2 - 1, ty = at.y * 2 - 1;
    float l = sqrt(tx * tx + ty * ty);
    if (l > 0) {
      float c = tx / l, s = ty / l;
      float rx = c * dirX - s * dirY, ry = s * dirX + c * dirY;
      dirX = rx;
      dirY = ry;
    }
    aniso *= at.z;
  }
  si.bsdf.anisotropy = aniso;
  if (hasTangent) {
    Frame tf = frameFromNormalTangent(si.ns, tangent);
    si.frame = frameFromNormalTangent(si.ns, tf.t * dirX + tf.b * (dirY * handed));
  } else {
    si.frame = frameFromNormal(si.ns);
  }
  si.front_face = dot(wo, si.ng) >= 0;
  return si;
}
