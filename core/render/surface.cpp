#include "core/render/surface.h"

#include <bit>
#include <cmath>

namespace pen {

namespace {

Vec4f lookup(const Scene& scene, const TextureRef& ref, Vec2f uv0, Vec2f uv1, bool srgb) {
  if (!ref.valid()) return {1, 1, 1, 1};
  const Vec2f uv = ref.apply(ref.tex_coord == 1 ? uv1 : uv0);
  return scene.textures[ref.index].sample(uv, srgb);
}

template <class T>
T interp(const std::vector<T>& a, uint32_t i0, uint32_t i1, uint32_t i2, float b0, float b1, float b2) {
  return a[i0] * b0 + a[i1] * b1 + a[i2] * b2;
}

Vec4f interp4(const std::vector<Vec4f>& a, uint32_t i0, uint32_t i1, uint32_t i2, float b0, float b1, float b2) {
  const Vec4f x = a[i0], y = a[i1], z = a[i2];
  return {x.x * b0 + y.x * b1 + z.x * b2, x.y * b0 + y.y * b1 + z.y * b2, x.z * b0 + y.z * b1 + z.z * b2,
          x.w * b0 + y.w * b1 + z.w * b2};
}

}  // namespace

Vec3f offsetRayOrigin(Vec3f p, Vec3f n) {
  constexpr float kOrigin = 1.0f / 32.0f, kFloatScale = 1.0f / 65536.0f, kIntScale = 256.0f;
  Vec3f out;
  for (int i = 0; i < 3; ++i) {
    const int32_t off = static_cast<int32_t>(kIntScale * n[i]);
    const int32_t bits = std::bit_cast<int32_t>(p[i]) + (p[i] < 0 ? -off : off);
    out[i] = std::fabs(p[i]) < kOrigin ? p[i] + kFloatScale * n[i] : std::bit_cast<float>(bits);
  }
  return out;
}

float alphaAt(const Scene& scene, uint32_t instance, uint32_t primitive, uint32_t triangle, float u, float v) {
  const MeshPrimitive& prim = scene.meshes[scene.instances[instance].mesh].primitives[primitive];
  const Material& mat = scene.materials[prim.material];
  float alpha = mat.base_color.w;
  const uint32_t i0 = prim.indices[triangle * 3], i1 = prim.indices[triangle * 3 + 1], i2 = prim.indices[triangle * 3 + 2];
  const float b0 = 1 - u - v;
  if (!prim.colors.empty()) alpha *= interp4(prim.colors, i0, i1, i2, b0, u, v).w;
  if (mat.base_color_tex.valid()) {
    const Vec2f uv0 = prim.uv0.empty() ? Vec2f{} : interp(prim.uv0, i0, i1, i2, b0, u, v);
    const Vec2f uv1 = prim.uv1.empty() ? Vec2f{} : interp(prim.uv1, i0, i1, i2, b0, u, v);
    alpha *= lookup(scene, mat.base_color_tex, uv0, uv1, true).w;
  }
  return alpha;
}

SurfaceInteraction makeSurfaceInteraction(const Scene& scene, const Hit& hit, Vec3f wo) {
  const Instance& inst = scene.instances[hit.instance];
  const MeshPrimitive& prim = scene.meshes[inst.mesh].primitives[hit.primitive];
  const Material& mat = scene.materials[prim.material];
  const uint32_t i0 = prim.indices[hit.triangle * 3], i1 = prim.indices[hit.triangle * 3 + 1],
                 i2 = prim.indices[hit.triangle * 3 + 2];
  const float b1 = hit.u, b2 = hit.v, b0 = 1 - b1 - b2;

  SurfaceInteraction si;
  si.material = prim.material;
  si.double_sided = mat.double_sided;
  const Vec3f p0 = prim.positions[i0], p1 = prim.positions[i1], p2 = prim.positions[i2];
  si.p = inst.to_world.point(p0 * b0 + p1 * b1 + p2 * b2);
  // The normal transform (inverse transpose) keeps glTF's front face for mirrored instances too.
  si.ng = normalize(inst.to_world.normal(cross(p1 - p0, p2 - p0)));
  si.ns = prim.normals.empty() ? si.ng : normalize(inst.to_world.normal(interp(prim.normals, i0, i1, i2, b0, b1, b2)));
  if (dot(si.ns, si.ng) < 0) si.ns = -si.ns;  // interpolated normals may disagree with the winding
  si.uv0 = prim.uv0.empty() ? Vec2f{} : interp(prim.uv0, i0, i1, i2, b0, b1, b2);
  si.uv1 = prim.uv1.empty() ? Vec2f{} : interp(prim.uv1, i0, i1, i2, b0, b1, b2);

  Vec3f tangent(0);
  float handed = 1;
  if (!prim.tangents.empty()) {
    const Vec4f t = interp4(prim.tangents, i0, i1, i2, b0, b1, b2);
    tangent = inst.to_world.vector(t.xyz());
    handed = t.w < 0 ? -1.0f : 1.0f;
  }

  // ---- Material inputs ----
  BsdfInputs& in = si.bsdf;
  Vec4f base = mat.base_color;
  const Vec4f bt = lookup(scene, mat.base_color_tex, si.uv0, si.uv1, true);
  base = {base.x * bt.x, base.y * bt.y, base.z * bt.z, base.w * bt.w};
  if (!prim.colors.empty()) {
    const Vec4f c = interp4(prim.colors, i0, i1, i2, b0, b1, b2);
    base = {base.x * c.x, base.y * c.y, base.z * c.z, base.w * c.w};
  }
  in.base_color = base.xyz();
  si.alpha = base.w;
  const Vec4f mr = lookup(scene, mat.metallic_roughness_tex, si.uv0, si.uv1, false);
  in.metallic = mat.metallic * mr.z;
  in.roughness = mat.roughness * mr.y;
  in.transmission = mat.transmission * lookup(scene, mat.transmission_tex, si.uv0, si.uv1, false).x;
  in.ior = mat.ior;
  in.specular = mat.specular * lookup(scene, mat.specular_tex, si.uv0, si.uv1, false).w;
  in.specular_color = mat.specular_color * lookup(scene, mat.specular_color_tex, si.uv0, si.uv1, true).xyz();
  in.clearcoat = mat.clearcoat * lookup(scene, mat.clearcoat_tex, si.uv0, si.uv1, false).x;
  in.clearcoat_roughness = mat.clearcoat_roughness * lookup(scene, mat.clearcoat_roughness_tex, si.uv0, si.uv1, false).y;
  const Vec4f sc = lookup(scene, mat.sheen_color_tex, si.uv0, si.uv1, true);
  in.sheen_color = mat.sheen_color * sc.xyz();
  in.sheen_roughness = mat.sheen_roughness * lookup(scene, mat.sheen_roughness_tex, si.uv0, si.uv1, false).w;
  if (mat.emits())
    si.emission = mat.emissive * lookup(scene, mat.emissive_tex, si.uv0, si.uv1, true).xyz() * mat.emissive_strength;

  // ---- Normal map ----
  const bool hasTangent = lengthSquared(tangent) > 0;
  if (mat.normal_tex.valid() && hasTangent) {
    const Vec4f tn = lookup(scene, mat.normal_tex, si.uv0, si.uv1, false);
    const float nx = (tn.x * 2 - 1) * mat.normal_scale, ny = (tn.y * 2 - 1) * mat.normal_scale, nz = tn.z * 2 - 1;
    const Frame tf = Frame::fromNormalTangent(si.ns, tangent);
    const Vec3f bit = tf.b * handed;
    const Vec3f n = normalize(tf.t * nx + bit * ny + tf.n * nz);
    if (dot(n, si.ng) > 0) si.ns = n;
  }

  // ---- Anisotropy direction (KHR_materials_anisotropy) ----
  float aniso = mat.anisotropy;
  float dirX = std::cos(mat.anisotropy_rotation), dirY = std::sin(mat.anisotropy_rotation);
  if (mat.anisotropy_tex.valid()) {
    const Vec4f at = lookup(scene, mat.anisotropy_tex, si.uv0, si.uv1, false);
    const float tx = at.x * 2 - 1, ty = at.y * 2 - 1;
    const float l = std::sqrt(tx * tx + ty * ty);
    if (l > 0) {
      const float c = tx / l, s = ty / l;
      const float rx = c * dirX - s * dirY, ry = s * dirX + c * dirY;
      dirX = rx;
      dirY = ry;
    }
    aniso *= at.z;
  }
  in.anisotropy = aniso;
  if (hasTangent) {
    const Frame tf = Frame::fromNormalTangent(si.ns, tangent);
    si.frame = Frame::fromNormalTangent(si.ns, tf.t * dirX + tf.b * (dirY * handed));
  } else {
    si.frame = Frame(si.ns);
  }

  // ---- Orientation ----
  si.front_face = dot(wo, si.ng) >= 0;
  return si;
}

}  // namespace pen
