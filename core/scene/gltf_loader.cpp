// glTF 2.0 loader (cgltf). Converts meshes, instances (incl. EXT_mesh_gpu_instancing), materials, textures,
// cameras and KHR_lights_punctual into a pen::Scene in world space.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <array>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <set>

#include "core/image/image_io.h"
#include "core/scene/scene_loader.h"
#include "core/system/paths.h"

namespace pen {

namespace {

const std::set<std::string> kSupportedExtensions = {
    "KHR_materials_transmission", "KHR_materials_ior",        "KHR_materials_clearcoat",
    "KHR_materials_sheen",        "KHR_materials_specular",   "KHR_materials_emissive_strength",
    "KHR_materials_anisotropy",   "KHR_texture_transform",    "KHR_lights_punctual",
    "EXT_mesh_gpu_instancing",
};

std::vector<uint8_t> readAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw SceneError(fmt::format("cannot open scene '{}'", pathToUtf8(path)));
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::vector<uint8_t> decodeBase64(std::string_view s) {
  static const std::array<int8_t, 256> table = [] {
    std::array<int8_t, 256> t{};
    t.fill(-1);
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(chars[i])] = static_cast<int8_t>(i);
    return t;
  }();
  std::vector<uint8_t> out;
  out.reserve(s.size() * 3 / 4);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : s) {
    const int8_t v = table[static_cast<uint8_t>(c)];
    if (v < 0) continue;  // padding and whitespace
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
    }
  }
  return out;
}

Vec4f readVec4(const float* f) { return {f[0], f[1], f[2], f[3]}; }
Vec3f readVec3(const float* f) { return {f[0], f[1], f[2]}; }

struct Converter {
  const cgltf_data* data;
  std::filesystem::path base_dir;
  std::vector<std::string>& warnings;
  Scene scene;
  std::map<const cgltf_mesh*, uint32_t> mesh_index;
  std::map<const cgltf_image*, int32_t> image_texture;  // decoded image -> first texture using it
  std::map<const cgltf_texture*, int32_t> texture_index;
  int32_t default_material = -1;
  bool warned_skin = false, warned_morph = false, warned_mode = false;

  void warn(std::string msg) { warnings.push_back(std::move(msg)); }

  int32_t texture(const cgltf_texture* tex) {
    if (!tex) return -1;
    if (auto it = texture_index.find(tex); it != texture_index.end()) return it->second;
    const cgltf_image* img = tex->image;
    if (!img) {
      warn(fmt::format("texture '{}' has no PNG/JPEG image (KTX2/WebP are not supported); ignored",
                       tex->name ? tex->name : "?"));
      return texture_index[tex] = -1;
    }
    Texture t;
    try {
      if (auto it = image_texture.find(img); it != image_texture.end() && it->second >= 0) {
        t = scene.textures[it->second];  // same pixels, maybe another sampler
      } else if (img->buffer_view) {
        const auto* bytes = cgltf_buffer_view_data(img->buffer_view);
        t = decodeTexture({bytes, img->buffer_view->size}, img->name ? img->name : "embedded image");
      } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {
        const char* comma = std::strchr(img->uri, ',');
        if (!comma) throw ImageIoError("malformed data URI");
        const std::vector<uint8_t> bytes = decodeBase64(comma + 1);
        t = decodeTexture(bytes, img->name ? img->name : "embedded image");
      } else if (img->uri) {
        std::string uri = img->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        t = loadTextureFile(base_dir / pathFromUtf8(uri));
      } else {
        throw ImageIoError("image has neither data nor URI");
      }
    } catch (const ImageIoError& e) {
      warn(fmt::format("{}; texture ignored", e.what()));
      return texture_index[tex] = -1;
    }
    if (const cgltf_sampler* s = tex->sampler) {
      const auto wrap = [](int mode) {
        if (mode == 33071) return TextureWrap::Clamp;
        if (mode == 33648) return TextureWrap::Mirror;
        return TextureWrap::Repeat;
      };
      t.wrap_s = wrap(static_cast<int>(s->wrap_s));
      t.wrap_t = wrap(static_cast<int>(s->wrap_t));
      t.nearest = static_cast<int>(s->mag_filter) == 9728;
    }
    const int32_t index = static_cast<int32_t>(scene.textures.size());
    scene.textures.push_back(std::move(t));
    if (!image_texture.count(img)) image_texture[img] = index;
    return texture_index[tex] = index;
  }

  TextureRef ref(const cgltf_texture_view& view) {
    TextureRef r;
    r.index = texture(view.texture);
    if (r.index < 0) return r;
    r.tex_coord = static_cast<uint32_t>(view.texcoord);
    if (view.has_transform) {
      const cgltf_texture_transform& t = view.transform;
      r.has_transform = true;
      r.offset = {t.offset[0], t.offset[1]};
      r.rotation = t.rotation;
      r.scale = {t.scale[0], t.scale[1]};
      if (t.has_texcoord) r.tex_coord = static_cast<uint32_t>(t.texcoord);
    }
    if (r.tex_coord > 1) {
      warn(fmt::format("texture uses TEXCOORD_{}; only TEXCOORD_0 and TEXCOORD_1 are supported", r.tex_coord));
      r.tex_coord = 0;
    }
    return r;
  }

  void convertMaterials() {
    for (size_t i = 0; i < data->materials_count; ++i) {
      const cgltf_material& g = data->materials[i];
      Material m;
      m.name = g.name ? g.name : fmt::format("material {}", i);
      if (g.has_pbr_metallic_roughness) {
        const auto& pbr = g.pbr_metallic_roughness;
        m.base_color = readVec4(pbr.base_color_factor);
        m.base_color_tex = ref(pbr.base_color_texture);
        m.metallic = pbr.metallic_factor;
        m.roughness = pbr.roughness_factor;
        m.metallic_roughness_tex = ref(pbr.metallic_roughness_texture);
      } else if (g.has_pbr_specular_glossiness) {
        warn(fmt::format("material '{}' uses KHR_materials_pbrSpecularGlossiness (unsupported); using defaults",
                         m.name));
      }
      m.normal_tex = ref(g.normal_texture);
      m.normal_scale = g.normal_texture.texture ? g.normal_texture.scale : 1.0f;
      m.emissive = readVec3(g.emissive_factor);
      m.emissive_tex = ref(g.emissive_texture);
      if (g.has_emissive_strength) m.emissive_strength = g.emissive_strength.emissive_strength;
      if (g.has_transmission) {
        m.transmission = g.transmission.transmission_factor;
        m.transmission_tex = ref(g.transmission.transmission_texture);
      }
      if (g.has_ior) m.ior = g.ior.ior > 0 ? g.ior.ior : 1.5f;
      if (g.has_specular) {
        m.specular = g.specular.specular_factor;
        m.specular_tex = ref(g.specular.specular_texture);
        m.specular_color = readVec3(g.specular.specular_color_factor);
        m.specular_color_tex = ref(g.specular.specular_color_texture);
      }
      if (g.has_clearcoat) {
        m.clearcoat = g.clearcoat.clearcoat_factor;
        m.clearcoat_tex = ref(g.clearcoat.clearcoat_texture);
        m.clearcoat_roughness = g.clearcoat.clearcoat_roughness_factor;
        m.clearcoat_roughness_tex = ref(g.clearcoat.clearcoat_roughness_texture);
      }
      if (g.has_sheen) {
        m.sheen_color = readVec3(g.sheen.sheen_color_factor);
        m.sheen_color_tex = ref(g.sheen.sheen_color_texture);
        m.sheen_roughness = g.sheen.sheen_roughness_factor;
        m.sheen_roughness_tex = ref(g.sheen.sheen_roughness_texture);
      }
      if (g.has_anisotropy) {
        m.anisotropy = g.anisotropy.anisotropy_strength;
        m.anisotropy_rotation = g.anisotropy.anisotropy_rotation;
        m.anisotropy_tex = ref(g.anisotropy.anisotropy_texture);
      }
      switch (g.alpha_mode) {
        case cgltf_alpha_mode_mask: m.alpha_mode = AlphaMode::Mask; break;
        case cgltf_alpha_mode_blend: m.alpha_mode = AlphaMode::Blend; break;
        default: m.alpha_mode = AlphaMode::Opaque; break;
      }
      m.alpha_cutoff = g.alpha_cutoff;
      m.double_sided = g.double_sided;
      if (g.unlit) warn(fmt::format("material '{}' is unlit (KHR_materials_unlit unsupported); shaded as PBR", m.name));
      scene.materials.push_back(std::move(m));
    }
  }

  uint32_t defaultMaterial() {
    if (default_material < 0) {
      Material m;
      m.name = "default";
      m.metallic = 0;
      m.roughness = 0.5f;
      default_material = static_cast<int32_t>(scene.materials.size());
      scene.materials.push_back(m);
    }
    return static_cast<uint32_t>(default_material);
  }

  template <class T, int N>
  std::vector<T> unpack(const cgltf_accessor* acc) {
    std::vector<T> out(acc->count);
    std::vector<float> tmp(acc->count * N);
    cgltf_accessor_unpack_floats(acc, tmp.data(), tmp.size());
    for (size_t i = 0; i < acc->count; ++i) {
      if constexpr (N == 2) out[i] = {tmp[i * 2], tmp[i * 2 + 1]};
      if constexpr (N == 3) out[i] = {tmp[i * 3], tmp[i * 3 + 1], tmp[i * 3 + 2]};
      if constexpr (N == 4) out[i] = {tmp[i * 4], tmp[i * 4 + 1], tmp[i * 4 + 2], tmp[i * 4 + 3]};
    }
    return out;
  }

  static void generateTangents(MeshPrimitive& p) {
    const size_t n = p.positions.size();
    std::vector<Vec3f> tan(n, Vec3f(0)), bit(n, Vec3f(0));
    for (uint32_t t = 0; t < p.triangleCount(); ++t) {
      const uint32_t i0 = p.indices[t * 3], i1 = p.indices[t * 3 + 1], i2 = p.indices[t * 3 + 2];
      const Vec3f e1 = p.positions[i1] - p.positions[i0], e2 = p.positions[i2] - p.positions[i0];
      const Vec2f d1 = p.uv0[i1] - p.uv0[i0], d2 = p.uv0[i2] - p.uv0[i0];
      const float det = d1.x * d2.y - d2.x * d1.y;
      if (std::fabs(det) < 1e-20f) continue;
      const float r = 1.0f / det;
      const Vec3f sdir = (e1 * d2.y - e2 * d1.y) * r;
      const Vec3f tdir = (e2 * d1.x - e1 * d2.x) * r;
      for (uint32_t i : {i0, i1, i2}) {
        tan[i] += sdir;
        bit[i] += tdir;
      }
    }
    p.tangents.resize(n);
    for (size_t i = 0; i < n; ++i) {
      const Vec3f nn = p.normals.empty() ? Vec3f(0, 0, 1) : p.normals[i];
      const Vec3f t = normalize(tan[i] - nn * dot(nn, tan[i]));
      const float w = dot(cross(nn, t), bit[i]) < 0 ? -1.0f : 1.0f;
      p.tangents[i] = {t.x, t.y, t.z, w};
    }
  }

  uint32_t mesh(const cgltf_mesh* gm) {
    if (auto it = mesh_index.find(gm); it != mesh_index.end()) return it->second;
    Mesh m;
    m.name = gm->name ? gm->name : fmt::format("mesh {}", scene.meshes.size());
    for (size_t pi = 0; pi < gm->primitives_count; ++pi) {
      const cgltf_primitive& gp = gm->primitives[pi];
      if (gp.type != cgltf_primitive_type_triangles) {
        if (!warned_mode) warn(fmt::format("mesh '{}': only triangle primitives are rendered; others skipped", m.name));
        warned_mode = true;
        continue;
      }
      if (gp.has_draco_mesh_compression) {
        warn(fmt::format("mesh '{}': Draco compression is not supported; primitive skipped", m.name));
        continue;
      }
      if (gp.targets_count > 0 && !warned_morph) {
        warn("morph targets are not supported; base shapes are rendered");
        warned_morph = true;
      }
      MeshPrimitive p;
      for (size_t ai = 0; ai < gp.attributes_count; ++ai) {
        const cgltf_attribute& a = gp.attributes[ai];
        switch (a.type) {
          case cgltf_attribute_type_position: p.positions = unpack<Vec3f, 3>(a.data); break;
          case cgltf_attribute_type_normal: p.normals = unpack<Vec3f, 3>(a.data); break;
          case cgltf_attribute_type_tangent: p.tangents = unpack<Vec4f, 4>(a.data); break;
          case cgltf_attribute_type_texcoord:
            if (a.index == 0) p.uv0 = unpack<Vec2f, 2>(a.data);
            if (a.index == 1) p.uv1 = unpack<Vec2f, 2>(a.data);
            break;
          case cgltf_attribute_type_color:
            if (a.index == 0) {
              if (cgltf_num_components(a.data->type) == 4) {
                p.colors = unpack<Vec4f, 4>(a.data);
              } else {
                const auto rgb = unpack<Vec3f, 3>(a.data);
                p.colors.resize(rgb.size());
                for (size_t i = 0; i < rgb.size(); ++i) p.colors[i] = {rgb[i].x, rgb[i].y, rgb[i].z, 1};
              }
            }
            break;
          default: break;
        }
      }
      if (p.positions.empty()) continue;
      if (gp.indices) {
        p.indices.resize(gp.indices->count);
        for (size_t i = 0; i < gp.indices->count; ++i)
          p.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(gp.indices, i));
      } else {
        p.indices.resize(p.positions.size());
        for (size_t i = 0; i < p.indices.size(); ++i) p.indices[i] = static_cast<uint32_t>(i);
      }
      p.indices.resize(p.indices.size() / 3 * 3);
      for (uint32_t idx : p.indices) {
        if (idx >= p.positions.size()) throw SceneError(fmt::format("mesh '{}': index out of range", m.name));
      }
      p.material = gp.material ? static_cast<uint32_t>(gp.material - data->materials) : defaultMaterial();
      const Material& mat = scene.materials[p.material];
      if (p.normals.size() != p.positions.size()) p.normals.clear();
      if (p.tangents.size() != p.positions.size()) p.tangents.clear();
      if (p.uv0.size() != p.positions.size()) p.uv0.clear();
      if (p.uv1.size() != p.positions.size()) p.uv1.clear();
      if (p.colors.size() != p.positions.size()) p.colors.clear();
      if (mat.normal_tex.valid() && p.tangents.empty() && !p.uv0.empty()) generateTangents(p);
      m.primitives.push_back(std::move(p));
    }
    const uint32_t index = static_cast<uint32_t>(scene.meshes.size());
    scene.meshes.push_back(std::move(m));
    return mesh_index[gm] = index;
  }

  void addCamera(const cgltf_node* node, const cgltf_camera* gc, const Affine3f& world) {
    Camera c;
    c.name = node->name ? node->name : (gc->name ? gc->name : fmt::format("camera {}", scene.cameras.size()));
    c.to_world = world;
    if (gc->type == cgltf_camera_type_orthographic) {
      c.projection = CameraProjection::Orthographic;
      c.xmag = gc->data.orthographic.xmag;
      c.ymag = gc->data.orthographic.ymag;
    } else {
      c.yfov = gc->data.perspective.yfov;
    }
    scene.cameras.push_back(c);
    // Also remember the camera data name so either name can be selected with --camera.
    if (gc->name && node->name && std::strcmp(gc->name, node->name) != 0) {
      Camera alias = c;
      alias.name = gc->name;
      camera_aliases.push_back(alias);
    }
  }
  std::vector<Camera> camera_aliases;

  void addLight(const cgltf_light* gl, const Affine3f& world) {
    PunctualLight l;
    l.name = gl->name ? gl->name : fmt::format("light {}", scene.lights.size());
    l.position = world.point(Vec3f(0));
    l.direction = normalize(world.vector(Vec3f(0, 0, -1)));
    l.intensity = readVec3(gl->color) * (gl->intensity / 683.0f);  // DECISIONS D-023
    switch (gl->type) {
      case cgltf_light_type_directional: l.type = LightType::Directional; break;
      case cgltf_light_type_spot:
        l.type = LightType::Spot;
        l.cos_inner = std::cos(gl->spot_inner_cone_angle);
        l.cos_outer = std::cos(gl->spot_outer_cone_angle);
        if (l.cos_inner <= l.cos_outer) l.cos_inner = l.cos_outer + 1e-4f;
        break;
      default: l.type = LightType::Point; break;
    }
    scene.lights.push_back(l);
  }

  void visit(const cgltf_node* node, const Affine3f& parent) {
    float local[16];
    cgltf_node_transform_local(node, local);
    const Affine3f world = parent * Affine3f::fromColumnMajor(local);
    if (node->skin && !warned_skin) {
      warn("skinned meshes are rendered in their bind pose");
      warned_skin = true;
    }
    if (node->mesh) {
      const uint32_t mi = mesh(node->mesh);
      const std::string name = node->name ? node->name : scene.meshes[mi].name;
      if (node->has_mesh_gpu_instancing) {
        const cgltf_accessor *tr = nullptr, *rot = nullptr, *sc = nullptr;
        for (size_t i = 0; i < node->mesh_gpu_instancing.attributes_count; ++i) {
          const cgltf_attribute& a = node->mesh_gpu_instancing.attributes[i];
          if (a.name && std::strcmp(a.name, "TRANSLATION") == 0) tr = a.data;
          if (a.name && std::strcmp(a.name, "ROTATION") == 0) rot = a.data;
          if (a.name && std::strcmp(a.name, "SCALE") == 0) sc = a.data;
        }
        const size_t count = tr ? tr->count : (rot ? rot->count : (sc ? sc->count : 0));
        for (size_t k = 0; k < count; ++k) {
          float t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
          if (tr) cgltf_accessor_read_float(tr, k, t, 3);
          if (rot) cgltf_accessor_read_float(rot, k, r, 4);
          if (sc) cgltf_accessor_read_float(sc, k, s, 3);
          const Affine3f inst = Affine3f::translate(readVec3(t)) * Affine3f::rotate(readVec4(r)) *
                                Affine3f::scale(readVec3(s));
          scene.instances.push_back({fmt::format("{}[{}]", name, k), mi, world * inst});
        }
      } else {
        scene.instances.push_back({name, mi, world});
      }
    }
    if (node->camera) addCamera(node, node->camera, world);
    if (node->light) addLight(node->light, world);
    for (size_t i = 0; i < node->children_count; ++i) visit(node->children[i], world);
  }
};

}  // namespace

bool isSupportedGltfExtension(const std::string& name) { return kSupportedExtensions.count(name) > 0; }

Scene loadGltf(const std::filesystem::path& path, std::vector<std::string>& warnings) {
  const std::vector<uint8_t> bytes = readAll(path);
  cgltf_options options{};
  cgltf_data* data = nullptr;
  const std::string src = pathToUtf8(path);
  if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success)
    throw SceneError(fmt::format("'{}' is not a valid glTF file", src));
  struct Free {
    cgltf_data* d;
    ~Free() { cgltf_free(d); }
  } guard{data};
  if (cgltf_load_buffers(&options, data, src.c_str()) != cgltf_result_success)
    throw SceneError(fmt::format("cannot load the buffers of '{}' (missing .bin file?)", src));
  if (cgltf_validate(data) != cgltf_result_success) throw SceneError(fmt::format("'{}' failed glTF validation", src));

  for (size_t i = 0; i < data->extensions_used_count; ++i) {
    const std::string ext = data->extensions_used[i];
    if (isSupportedGltfExtension(ext)) continue;
    if (ext == "KHR_materials_volume")
      warnings.push_back("KHR_materials_volume: thickness and attenuation are not supported; transmissive "
                         "materials render as solid glass");
    else
      warnings.push_back(fmt::format("glTF extension {} is not supported; falling back to base PBR", ext));
  }

  Converter conv{data, path.parent_path(), warnings, {}, {}, {}, {}, -1, false, false, false, {}};
  conv.convertMaterials();
  const cgltf_scene* gs = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
  if (gs) {
    for (size_t i = 0; i < gs->nodes_count; ++i) conv.visit(gs->nodes[i], Affine3f::identity());
  } else {
    for (size_t i = 0; i < data->nodes_count; ++i)
      if (!data->nodes[i].parent) conv.visit(&data->nodes[i], Affine3f::identity());
  }
  // Aliases (camera data names) go after the real cameras so "first camera" stays the first node camera.
  for (auto& c : conv.camera_aliases) conv.scene.cameras.push_back(std::move(c));
  if (conv.scene.materials.empty()) conv.defaultMaterial();
  return std::move(conv.scene);
}

}  // namespace pen
