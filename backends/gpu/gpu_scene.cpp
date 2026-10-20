#include "backends/gpu/gpu_scene.h"

#include "core/image/image.h"
#include "core/shading/bsdf.h"

namespace pen {

namespace {

void copy3(float* d, Vec3f v) {
  d[0] = v.x;
  d[1] = v.y;
  d[2] = v.z;
}

gpu::TextureRef ref(const TextureRef& r) {
  gpu::TextureRef g;
  g.index = r.index;
  g.tex_coord = r.tex_coord;
  g.has_transform = r.has_transform ? 1 : 0;
  g.offset[0] = r.offset.x;
  g.offset[1] = r.offset.y;
  g.rotation = r.rotation;
  g.scale[0] = r.scale.x;
  g.scale[1] = r.scale.y;
  return g;
}

gpu::Material material(const Material& m) {
  gpu::Material g{};
  g.base_color[0] = m.base_color.x;
  g.base_color[1] = m.base_color.y;
  g.base_color[2] = m.base_color.z;
  g.base_color[3] = m.base_color.w;
  g.base_color_tex = ref(m.base_color_tex);
  g.metallic = m.metallic;
  g.roughness = m.roughness;
  g.metallic_roughness_tex = ref(m.metallic_roughness_tex);
  g.normal_tex = ref(m.normal_tex);
  g.normal_scale = m.normal_scale;
  copy3(g.emissive, m.emissive);
  g.emissive_strength = m.emissive_strength;
  g.emissive_tex = ref(m.emissive_tex);
  g.transmission = m.transmission;
  g.transmission_tex = ref(m.transmission_tex);
  g.ior = m.ior;
  g.specular = m.specular;
  g.specular_tex = ref(m.specular_tex);
  copy3(g.specular_color, m.specular_color);
  g.specular_color_tex = ref(m.specular_color_tex);
  g.clearcoat = m.clearcoat;
  g.clearcoat_tex = ref(m.clearcoat_tex);
  g.clearcoat_roughness = m.clearcoat_roughness;
  g.clearcoat_roughness_tex = ref(m.clearcoat_roughness_tex);
  copy3(g.sheen_color, m.sheen_color);
  g.sheen_color_tex = ref(m.sheen_color_tex);
  g.sheen_roughness = m.sheen_roughness;
  g.sheen_roughness_tex = ref(m.sheen_roughness_tex);
  g.anisotropy = m.anisotropy;
  g.anisotropy_rotation = m.anisotropy_rotation;
  g.anisotropy_tex = ref(m.anisotropy_tex);
  g.alpha_mode = static_cast<uint32_t>(m.alpha_mode);
  g.alpha_cutoff = m.alpha_cutoff;
  g.double_sided = m.double_sided ? 1 : 0;
  g.emits = m.emits() ? 1 : 0;
  return g;
}

void appendTable(std::vector<gpu::AliasEntry>& out, const AliasTable& t) {
  for (uint32_t i = 0; i < t.size(); ++i) out.push_back({t.prob(i), t.alias(i), t.pmf(i)});
}

}  // namespace

GpuScene flattenScene(const Scene& scene) {
  GpuScene g;

  // ---- Geometry ----
  for (const Mesh& mesh : scene.meshes) {
    g.mesh_primitive_base.push_back(static_cast<uint32_t>(g.primitives.size()));
    for (const MeshPrimitive& p : mesh.primitives) {
      gpu::Primitive gp{};
      gp.index_offset = static_cast<uint32_t>(g.indices.size());
      gp.position_offset = static_cast<uint32_t>(g.positions.size());
      gp.material = p.material;
      const auto attr = [](auto& dst, const auto& src) {
        if (src.empty()) return -1;
        const int32_t off = static_cast<int32_t>(dst.size());
        dst.insert(dst.end(), src.begin(), src.end());
        return off;
      };
      g.positions.insert(g.positions.end(), p.positions.begin(), p.positions.end());
      g.indices.insert(g.indices.end(), p.indices.begin(), p.indices.end());
      gp.normal_offset = attr(g.normals, p.normals);
      gp.tangent_offset = attr(g.tangents, p.tangents);
      gp.uv0_offset = attr(g.uvs, p.uv0);
      gp.uv1_offset = attr(g.uvs, p.uv1);
      gp.color_offset = attr(g.colors, p.colors);
      g.primitives.push_back(gp);
    }
  }

  for (uint32_t i = 0; i < scene.instances.size(); ++i) {
    const Instance& inst = scene.instances[i];
    gpu::Instance gi{};
    const Affine3f inv = inst.to_world.inverse();
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) {
        gi.to_world[r * 4 + c] = inst.to_world.m[r][c];
        gi.inverse[r * 4 + c] = inv.m[r][c];
      }
    gi.mesh = inst.mesh;
    gi.primitive_base = g.mesh_primitive_base[inst.mesh];
    gi.slot_base = static_cast<uint32_t>(g.emissive_offset.size());
    const auto& slots = scene.emissive_offset[i];
    g.emissive_offset.insert(g.emissive_offset.end(), slots.begin(), slots.end());
    g.instances.push_back(gi);
  }

  // ---- Materials and textures ----
  for (const Material& m : scene.materials) g.materials.push_back(material(m));
  for (const Texture& t : scene.textures) {
    gpu::Texture gt{};
    gt.width = t.width;
    gt.height = t.height;
    gt.is_float = t.is_float ? 1 : 0;
    gt.wrap_s = static_cast<uint32_t>(t.wrap_s);
    gt.wrap_t = static_cast<uint32_t>(t.wrap_t);
    gt.nearest = t.nearest ? 1 : 0;
    if (t.is_float) {
      gt.offset = static_cast<uint32_t>(g.texels32.size());
      for (size_t i = 0; i + 3 < t.hdr.size(); i += 4)
        g.texels32.push_back({t.hdr[i], t.hdr[i + 1], t.hdr[i + 2], t.hdr[i + 3]});
    } else {
      gt.offset = static_cast<uint32_t>(g.texels8.size());
      for (size_t i = 0; i + 3 < t.ldr.size(); i += 4)
        g.texels8.push_back(uint32_t(t.ldr[i]) | (uint32_t(t.ldr[i + 1]) << 8) | (uint32_t(t.ldr[i + 2]) << 16) |
                            (uint32_t(t.ldr[i + 3]) << 24));
    }
    g.textures.push_back(gt);
  }

  // ---- Lights ----
  for (const EmissiveTriangle& e : scene.emissive_triangles) {
    gpu::EmissiveTriangle ge{};
    copy3(ge.p0, e.p0);
    copy3(ge.e1, e.e1);
    copy3(ge.e2, e.e2);
    copy3(ge.normal, e.normal);
    ge.area = e.area;
    ge.instance = e.instance;
    ge.primitive = e.primitive;
    ge.triangle = e.triangle;
    ge.double_sided = e.double_sided ? 1 : 0;
    g.emissive.push_back(ge);
  }
  appendTable(g.light_table, scene.light_table);
  for (const PunctualLight& l : scene.lights) {
    gpu::PunctualLight gl{};
    gl.type = static_cast<uint32_t>(l.type);
    copy3(gl.position, l.position);
    copy3(gl.direction, l.direction);
    copy3(gl.intensity, l.intensity);
    gl.cos_inner = l.cos_inner;
    gl.cos_outer = l.cos_outer;
    g.lights.push_back(gl);
  }
  const Environment& env = scene.environment;
  if (env.hasMap() && !env.sampling.empty()) {
    appendTable(g.env_tables, env.sampling.rows());
    for (uint32_t v = 0; v < env.sampling.nv(); ++v) appendTable(g.env_tables, env.sampling.row(v));
  }
  g.env_map = &env.map.pixels;
  static const std::vector<float> kNone;
  g.env_weights = env.hasMap() ? &env.sampling.weights() : &kNone;
  g.sheen_table.assign(sheenAlbedoTable(), sheenAlbedoTable() + kSheenTableSize * kSheenTableSize);
  g.srgb_table.assign(srgbToLinearTable(), srgbToLinearTable() + 256);
  return g;
}

gpu::Frame makeGpuFrame(const FrameSetup& frame, const Scene& scene) {
  gpu::Frame f{};
  const Camera& cam = frame.camera.camera();
  f.projection = static_cast<uint32_t>(cam.projection);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) f.cam_to_world[r * 4 + c] = cam.to_world.m[r][c];
  f.ymag = cam.ymag;
  f.aperture_radius = cam.aperture_radius;
  f.focus_distance = cam.focus_distance;
  f.width = frame.camera.width();
  f.height = frame.camera.height();
  f.tan_x = frame.camera.tanX();
  f.tan_y = frame.camera.tanY();
  const IntegratorParams& p = frame.params;
  f.mode = static_cast<uint32_t>(p.mode);
  f.max_bounces = p.max_bounces;
  f.max_diffuse = p.max_diffuse;
  f.max_glossy = p.max_glossy;
  f.max_transmission = p.max_transmission;
  f.russian_roulette = p.russian_roulette ? 1 : 0;
  f.ao_distance = p.ao_distance;
  f.seed = frame.seed;
  f.num_emissive = static_cast<uint32_t>(scene.emissive_triangles.size());
  f.num_punctual = static_cast<uint32_t>(scene.lights.size());
  f.light_count = scene.light_table.size();
  f.light_sum = scene.light_table.sum();
  const Environment& env = scene.environment;
  f.env_has_map = env.hasMap() ? 1 : 0;
  f.env_width = env.map.width;
  f.env_height = env.map.height;
  f.env_strength = env.strength;
  f.env_rotation = env.rotation;
  copy3(f.env_color, env.color);
  f.env_is_black = env.isBlack() ? 1 : 0;
  f.env_dist_sum = env.hasMap() ? env.sampling.sum() : 0;
  f.env_light_index = scene.environmentLightIndex();
  return f;
}

}  // namespace pen
