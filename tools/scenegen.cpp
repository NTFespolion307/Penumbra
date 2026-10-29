// penumbra-scenegen <output folder>
// Writes the test scenes (glTF with embedded buffers) and their .pen.json files:
//   cornell.gltf          classic Cornell box (Lambert walls, area light)            -> cornell.pen.json
//   furnace_diffuse.gltf  white Lambert sphere in a uniform white environment         -> furnace_diffuse.pen.json
//   furnace_glass.gltf    smooth glass sphere in a uniform white environment          -> furnace_glass.pen.json
//   furnace_fog.gltf      white Lambert ground under non-absorbing height fog, white environment (M14)
//                         -> furnace_fog.pen.json
//   spheres.gltf + sky.hdr glass, gold, clear-coated and sheen spheres on a procedural HDRI -> spheres.pen.json
//   bigtex.gltf + bigtex.bin + textures/noise.png   a quad with a 12 MB noise texture, external buffer and a
//                         texture in a subfolder: scene package transfer tests -> bigtex.pen.json
//   cutout.gltf           orthographic camera (axis-parallel rays), an alpha-masked checker quad, one sphere mesh
//                         instanced five times, 1- and 2-triangle meshes: GPU traversal tests -> cutout.pen.json
// The output is deterministic, so tests can regenerate it at build time.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "core/math/vec.h"

namespace fs = std::filesystem;
using nlohmann::json;
using pen::Vec3f;

namespace {

std::string base64(const std::vector<uint8_t>& data) {
  static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t n = (uint32_t(data[i]) << 16) | (i + 1 < data.size() ? uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < data.size() ? uint32_t(data[i + 2]) : 0);
    out += chars[(n >> 18) & 63];
    out += chars[(n >> 12) & 63];
    out += i + 1 < data.size() ? chars[(n >> 6) & 63] : '=';
    out += i + 2 < data.size() ? chars[n & 63] : '=';
  }
  return out;
}

struct MeshData {
  std::vector<Vec3f> positions, normals;
  std::vector<float> uvs;  // optional TEXCOORD_0, 2 floats per vertex
  std::vector<uint32_t> indices;
};

class GltfWriter {
 public:
  GltfWriter() {
    doc_["asset"] = {{"version", "2.0"}, {"generator", "penumbra-scenegen"}};
    doc_["scenes"] = json::array({{{"nodes", json::array()}}});
    doc_["scene"] = 0;
  }

  int material(json m) { return push("materials", std::move(m)); }
  int camera(json c) { return push("cameras", std::move(c)); }

  int mesh(const std::string& name, const MeshData& d, int material) {
    const int pos = accessor(d.positions.data(), d.positions.size() * 12, d.positions.size(), 5126, "VEC3", 34962,
                             &d.positions);
    const int nrm = accessor(d.normals.data(), d.normals.size() * 12, d.normals.size(), 5126, "VEC3", 34962, nullptr);
    const int idx = accessor(d.indices.data(), d.indices.size() * 4, d.indices.size(), 5125, "SCALAR", 34963, nullptr);
    json prim = {{"attributes", {{"POSITION", pos}, {"NORMAL", nrm}}}, {"indices", idx}, {"material", material}};
    if (!d.uvs.empty())
      prim["attributes"]["TEXCOORD_0"] =
          accessor(d.uvs.data(), d.uvs.size() * 4, d.uvs.size() / 2, 5126, "VEC2", 34962, nullptr);
    return push("meshes", {{"name", name}, {"primitives", json::array({prim})}});
  }

  void node(json n) {
    const int i = push("nodes", std::move(n));
    doc_["scenes"][0]["nodes"].push_back(i);
  }

  void useExtension(const std::string& e) {
    auto& used = doc_["extensionsUsed"];
    if (!used.is_array()) used = json::array();
    for (const auto& u : used)
      if (u == e) return;
    used.push_back(e);
  }

  int texture(const std::string& uri) {
    const int image = push("images", {{"uri", uri}});
    return push("textures", {{"source", image}});
  }

  // Embedded buffer (default), or an external .bin next to the .gltf.
  void write(const fs::path& path, const std::string& binName = "") {
    if (binName.empty()) {
      doc_["buffers"] = json::array(
          {{{"byteLength", bin_.size()}, {"uri", "data:application/octet-stream;base64," + base64(bin_)}}});
    } else {
      doc_["buffers"] = json::array({{{"byteLength", bin_.size()}, {"uri", binName}}});
      std::ofstream(path.parent_path() / binName, std::ios::binary)
          .write(reinterpret_cast<const char*>(bin_.data()), static_cast<std::streamsize>(bin_.size()));
    }
    std::ofstream(path, std::ios::binary) << doc_.dump(1) << "\n";
  }

 private:
  int push(const char* key, json v) {
    auto& arr = doc_[key];
    if (!arr.is_array()) arr = json::array();
    arr.push_back(std::move(v));
    return static_cast<int>(arr.size() - 1);
  }

  int accessor(const void* data, size_t bytes, size_t count, int componentType, const char* type, int target,
               const std::vector<Vec3f>* bounds) {
    while (bin_.size() % 4) bin_.push_back(0);
    const size_t offset = bin_.size();
    const auto* p = static_cast<const uint8_t*>(data);
    bin_.insert(bin_.end(), p, p + bytes);
    const int view = push("bufferViews", {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}, {"target", target}});
    json a = {{"bufferView", view}, {"componentType", componentType}, {"count", count}, {"type", type}};
    if (bounds) {
      Vec3f lo(1e30f), hi(-1e30f);
      for (const Vec3f& v : *bounds) {
        lo = pen::vmin(lo, v);
        hi = pen::vmax(hi, v);
      }
      a["min"] = {lo.x, lo.y, lo.z};
      a["max"] = {hi.x, hi.y, hi.z};
    }
    return push("accessors", std::move(a));
  }

  json doc_;
  std::vector<uint8_t> bin_;
};

// Quad a-b-c-d as two triangles, wound so the face normal points toward `toward` (a point the face looks at).
void addQuad(MeshData& m, Vec3f a, Vec3f b, Vec3f c, Vec3f d, Vec3f toward, float scale) {
  a = a * scale, b = b * scale, c = c * scale, d = d * scale;
  Vec3f n = pen::normalize(pen::cross(b - a, c - a));
  const Vec3f center = (a + b + c + d) * 0.25f;
  if (pen::dot(n, toward * scale - center) < 0) {
    std::swap(b, d);
    n = -n;
  }
  const uint32_t base = static_cast<uint32_t>(m.positions.size());
  for (const Vec3f& v : {a, b, c, d}) {
    m.positions.push_back(v);
    m.normals.push_back(n);
  }
  for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

// Faces of a box given as quads, oriented outward from its centre.
void addBlock(MeshData& m, const std::vector<std::array<Vec3f, 4>>& quads, float scale) {
  Vec3f c(0);
  for (const auto& q : quads)
    for (const Vec3f& v : q) c += v;
  c = c / static_cast<float>(quads.size() * 4);
  for (const auto& q : quads) {
    const Vec3f qc = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
    addQuad(m, q[0], q[1], q[2], q[3], qc + (qc - c), scale);
  }
}

MeshData uvSphere(float radius, uint32_t segments, uint32_t rings) {
  MeshData m;
  for (uint32_t r = 0; r <= rings; ++r) {
    const float theta = pen::kPi * r / rings;
    for (uint32_t s = 0; s <= segments; ++s) {
      const float phi = pen::kTwoPi * s / segments;
      const Vec3f n{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
      m.positions.push_back(n * radius);
      m.normals.push_back(n);
    }
  }
  for (uint32_t r = 0; r < rings; ++r) {
    for (uint32_t s = 0; s < segments; ++s) {
      const uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
      if (r != 0) m.indices.insert(m.indices.end(), {a, b, a + 1});
      if (r != rings - 1) m.indices.insert(m.indices.end(), {a + 1, b, b + 1});
    }
  }
  return m;
}

json lambert(const char* name, Vec3f c) {
  return {{"name", name},
          {"pbrMetallicRoughness", {{"baseColorFactor", {c.x, c.y, c.z, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
          {"extensions", {{"KHR_materials_specular", {{"specularFactor", 0.0}}}}},
          {"doubleSided", true}};
}

void writeJson(const fs::path& path, const json& j) { std::ofstream(path, std::ios::binary) << j.dump(2) << "\n"; }

void cornell(const fs::path& dir) {
  GltfWriter w;
  w.useExtension("KHR_materials_specular");
  w.useExtension("KHR_materials_emissive_strength");
  const int white = w.material(lambert("white", {0.725f, 0.71f, 0.68f}));
  const int red = w.material(lambert("red", {0.63f, 0.065f, 0.05f}));
  const int green = w.material(lambert("green", {0.14f, 0.45f, 0.091f}));
  const int light = w.material({{"name", "light"},
                                {"pbrMetallicRoughness", {{"baseColorFactor", {0, 0, 0, 1}}, {"metallicFactor", 0.0}}},
                                {"emissiveFactor", {1.0, 12.0 / 17.0, 4.0 / 17.0}},
                                {"extensions", {{"KHR_materials_emissive_strength", {{"emissiveStrength", 17.0}}}}}});
  constexpr float s = 0.01f;  // millimetres of the original measurements -> scene units
  const Vec3f inside{278, 274, 280};
  MeshData walls, redWall, greenWall, lamp, shortBlock, tallBlock;
  addQuad(walls, {552.8f, 0, 0}, {0, 0, 0}, {0, 0, 559.2f}, {549.6f, 0, 559.2f}, inside, s);              // floor
  addQuad(walls, {556, 548.8f, 0}, {556, 548.8f, 559.2f}, {0, 548.8f, 559.2f}, {0, 548.8f, 0}, inside, s);  // ceiling
  addQuad(walls, {549.6f, 0, 559.2f}, {0, 0, 559.2f}, {0, 548.8f, 559.2f}, {556, 548.8f, 559.2f}, inside, s);  // back
  addQuad(greenWall, {0, 0, 559.2f}, {0, 0, 0}, {0, 548.8f, 0}, {0, 548.8f, 559.2f}, inside, s);
  addQuad(redWall, {552.8f, 0, 0}, {549.6f, 0, 559.2f}, {556, 548.8f, 559.2f}, {556, 548.8f, 0}, inside, s);
  addQuad(lamp, {343, 548.7f, 227}, {343, 548.7f, 332}, {213, 548.7f, 332}, {213, 548.7f, 227}, {278, 0, 280}, s);
  addBlock(shortBlock,
           {{{{130, 165, 65}, {82, 165, 225}, {240, 165, 272}, {290, 165, 114}}},
            {{{290, 0, 114}, {290, 165, 114}, {240, 165, 272}, {240, 0, 272}}},
            {{{130, 0, 65}, {130, 165, 65}, {290, 165, 114}, {290, 0, 114}}},
            {{{82, 0, 225}, {82, 165, 225}, {130, 165, 65}, {130, 0, 65}}},
            {{{240, 0, 272}, {240, 165, 272}, {82, 165, 225}, {82, 0, 225}}}},
           s);
  addBlock(tallBlock,
           {{{{423, 330, 247}, {265, 330, 296}, {314, 330, 456}, {472, 330, 406}}},
            {{{423, 0, 247}, {423, 330, 247}, {472, 330, 406}, {472, 0, 406}}},
            {{{472, 0, 406}, {472, 330, 406}, {314, 330, 456}, {314, 0, 456}}},
            {{{314, 0, 456}, {314, 330, 456}, {265, 330, 296}, {265, 0, 296}}},
            {{{265, 0, 296}, {265, 330, 296}, {423, 330, 247}, {423, 0, 247}}}},
           s);
  w.node({{"name", "walls"}, {"mesh", w.mesh("walls", walls, white)}});
  w.node({{"name", "red wall"}, {"mesh", w.mesh("red wall", redWall, red)}});
  w.node({{"name", "green wall"}, {"mesh", w.mesh("green wall", greenWall, green)}});
  w.node({{"name", "light"}, {"mesh", w.mesh("light", lamp, light)}});
  w.node({{"name", "short block"}, {"mesh", w.mesh("short block", shortBlock, white)}});
  w.node({{"name", "tall block"}, {"mesh", w.mesh("tall block", tallBlock, white)}});
  // Camera at (278, 273, -800) looking down +Z: glTF cameras look down -Z, so rotate 180 degrees about Y.
  const int cam = w.camera({{"type", "perspective"}, {"name", "Camera"},
                            {"perspective", {{"yfov", 39.3077 * 3.14159265358979 / 180}, {"znear", 0.01}}}});
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {2.78, 2.73, -8.0}}, {"rotation", {0, 1, 0, 0}}});
  w.write(dir / "cornell.gltf");
  writeJson(dir / "cornell.pen.json", {{"scene", "cornell.gltf"}, {"render", {{"res", "512x512"}}}});
}

void sphereScene(const fs::path& dir, const std::string& name, const json& mat, const json& render) {
  GltfWriter w;
  if (mat.contains("extensions"))
    for (const auto& [ext, v] : mat["extensions"].items()) w.useExtension(ext);
  const int m = w.material(mat);
  w.node({{"name", "sphere"}, {"mesh", w.mesh("sphere", uvSphere(1, 512, 256), m)}});
  const int cam = w.camera({{"type", "perspective"}, {"perspective", {{"yfov", 0.6}, {"znear", 0.01}}}});
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {0, 0, 4.0}}});
  w.write(dir / (name + ".gltf"));
  writeJson(dir / (name + ".pen.json"),
            {{"scene", name + ".gltf"}, {"environment", {{"color", {1.0, 1.0, 1.0}}}}, {"render", render}});
}

// The atmosphere's furnace (M14): a white Lambert ground 20 km wide under height fog that scatters without
// absorbing, in a uniform white environment. Everything conserves energy, so every pixel is 1. The ground stops
// rays going down, where the fog keeps getting denser (as real terrain would).
void furnaceFog(const fs::path& dir) {
  GltfWriter w;
  w.useExtension("KHR_materials_specular");
  const int white = w.material(lambert("white", {1, 1, 1}));
  MeshData ground;
  addQuad(ground, {-1e4f, 0, -1e4f}, {1e4f, 0, -1e4f}, {1e4f, 0, 1e4f}, {-1e4f, 0, 1e4f}, {0, 10, 0}, 1);
  w.node({{"name", "ground"}, {"mesh", w.mesh("ground", ground, white)}});
  const int cam = w.camera({{"type", "perspective"}, {"perspective", {{"yfov", 0.9}, {"znear", 0.01}}}});
  // 3 units up, looking 10 degrees down, so the image holds ground, fog and the horizon.
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {0, 3.0, 10.0}}, {"rotation", {-0.0871557, 0, 0, 0.9961947}}});
  w.write(dir / "furnace_fog.gltf");
  writeJson(dir / "furnace_fog.pen.json",
            {{"scene", "furnace_fog.gltf"},
             {"environment", {{"color", {1.0, 1.0, 1.0}}}},
             {"atmosphere", {{"air", 0.0}, {"haze", 0.0}, {"fog_density", 0.05}, {"fog_height", 20.0}, {"fog_base", 0.0},
                             {"fog_anisotropy", 0.3}, {"fog_color", {1.0, 1.0, 1.0}}}},
             {"render", {{"res", "64x64"}, {"max_bounces", 512}}}});
}

void skyHdr(const fs::path& path) {
  constexpr int W = 512, H = 256;
  std::vector<float> px(W * H * 3);
  const Vec3f sunDir = pen::normalize(Vec3f(0.4f, 0.55f, 0.6f));
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      // Same mapping as the renderer's environment lookup (Blender-compatible equirectangular).
      const float phi = ((x + 0.5f) / W - 0.5f) * pen::kTwoPi, theta = (y + 0.5f) / H * pen::kPi;
      const Vec3f d{-std::cos(phi) * std::sin(theta), std::cos(theta), -std::sin(phi) * std::sin(theta)};
      Vec3f c;
      if (d.y > 0) c = pen::lerp(Vec3f(0.9f, 0.95f, 1.0f), Vec3f(0.25f, 0.45f, 0.9f), std::pow(d.y, 0.5f));
      else c = Vec3f(0.3f, 0.27f, 0.22f) * (1 + d.y * 0.5f);
      if (pen::dot(d, sunDir) > 0.9995f) c = Vec3f(300, 270, 225);
      px[(y * W + x) * 3] = c.x;
      px[(y * W + x) * 3 + 1] = c.y;
      px[(y * W + x) * 3 + 2] = c.z;
    }
  }
  stbi_write_hdr(path.string().c_str(), W, H, 3, px.data());
}

void spheres(const fs::path& dir) {
  GltfWriter w;
  for (const char* e : {"KHR_materials_transmission", "KHR_materials_ior", "KHR_materials_clearcoat", "KHR_materials_sheen"})
    w.useExtension(e);
  const int ground = w.material({{"name", "ground"},
                                 {"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 0.5, 0.5, 1}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.8}}}});
  const int glass = w.material({{"name", "glass"},
                                {"pbrMetallicRoughness", {{"metallicFactor", 0.0}, {"roughnessFactor", 0.0}}},
                                {"extensions", {{"KHR_materials_transmission", {{"transmissionFactor", 1.0}}}, {"KHR_materials_ior", {{"ior", 1.5}}}}}});
  const int gold = w.material({{"name", "gold"},
                               {"pbrMetallicRoughness", {{"baseColorFactor", {1.0, 0.766, 0.336, 1}}, {"metallicFactor", 1.0}, {"roughnessFactor", 0.25}}}});
  const int coated = w.material({{"name", "red clearcoat"},
                                 {"pbrMetallicRoughness", {{"baseColorFactor", {0.6, 0.05, 0.05, 1}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.6}}},
                                 {"extensions", {{"KHR_materials_clearcoat", {{"clearcoatFactor", 1.0}, {"clearcoatRoughnessFactor", 0.03}}}}}});
  const int cloth = w.material({{"name", "sheen cloth"},
                                {"pbrMetallicRoughness", {{"baseColorFactor", {0.1, 0.1, 0.35, 1}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.9}}},
                                {"extensions", {{"KHR_materials_sheen", {{"sheenColorFactor", {1.0, 0.8, 0.9}}, {"sheenRoughnessFactor", 0.4}}}}}});
  MeshData plane;
  addQuad(plane, {-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20}, {0, 10, 0}, 1);
  w.node({{"name", "ground"}, {"mesh", w.mesh("ground", plane, ground)}});
  const MeshData ball = uvSphere(1, 128, 64);
  const int mats[4] = {glass, gold, coated, cloth};
  const char* names[4] = {"glass", "gold", "clearcoat", "sheen"};
  for (int i = 0; i < 4; ++i) {
    const int mesh = w.mesh(names[i], ball, mats[i]);
    w.node({{"name", names[i]}, {"mesh", mesh}, {"translation", {-3.3 + 2.2 * i, 1.0, 0.0}}});
  }
  // Slightly above, looking a little down (rotation of -10 degrees about X).
  const int cam = w.camera({{"type", "perspective"}, {"perspective", {{"yfov", 0.5}, {"znear", 0.01}}}});
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {0, 2.2, 10.0}}, {"rotation", {-0.0871557, 0, 0, 0.9961947}}});
  w.write(dir / "spheres.gltf");
  skyHdr(dir / "sky.hdr");
  writeJson(dir / "spheres.pen.json", {{"scene", "spheres.gltf"},
                                        {"environment", {{"hdri", "sky.hdr"}, {"strength", 1.0}, {"rotation_deg", 0}}},
                                        {"render", {{"res", "720p"}}}});
}

// A lit quad whose base colour is a 2048x2048 noise PNG (about 12 MB, barely compressible): big enough for the
// scene transfer to take several 4 MB chunks.
void bigtex(const fs::path& dir) {
  constexpr int N = 2048;
  std::vector<uint8_t> px(size_t(N) * N * 3);
  uint32_t state = 12345;
  for (auto& v : px) {
    state = state * 1664525u + 1013904223u;
    v = static_cast<uint8_t>(state >> 24);
  }
  fs::create_directories(dir / "textures");
  stbi_write_png((dir / "textures" / "noise.png").string().c_str(), N, N, 3, px.data(), N * 3);

  GltfWriter w;
  const int tex = w.texture("textures/noise.png");
  const int mat = w.material({{"name", "noise"},
                              {"pbrMetallicRoughness",
                               {{"baseColorTexture", {{"index", tex}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.7}}}});
  MeshData quad;
  addQuad(quad, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {0, 0, 5}, 1);
  quad.uvs = {0, 1, 1, 1, 1, 0, 0, 0};
  w.node({{"name", "quad"}, {"mesh", w.mesh("quad", quad, mat)}});
  const int cam = w.camera({{"type", "perspective"}, {"perspective", {{"yfov", 0.8}, {"znear", 0.01}}}});
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {0, 0, 3.0}}});
  w.write(dir / "bigtex.gltf", "bigtex.bin");
  writeJson(dir / "bigtex.pen.json",
            {{"scene", "bigtex.gltf"}, {"environment", {{"color", {1.0, 1.0, 1.0}}}}, {"render", {{"res", "240p"}}}});
}

// Software ray tracing corner cases (CUDA backend, DECISIONS D-086): rays with exact zero direction components
// (orthographic camera looking down -Z), alpha cutouts inside the traversal, instancing with non-uniform scale and
// rotation (TLAS and object-space rays), and meshes too small for a CWBVH (triangle lists).
void cutout(const fs::path& dir) {
  constexpr int N = 64;
  std::vector<uint8_t> px(size_t(N) * N * 4);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      uint8_t* p = &px[(size_t(y) * N + x) * 4];
      const bool open = ((x / 8) + (y / 8)) % 2 == 0;
      p[0] = 230, p[1] = 140, p[2] = 40, p[3] = open ? 0 : 255;
    }
  stbi_write_png((dir / "cutout_checker.png").string().c_str(), N, N, 4, px.data(), N * 4);

  GltfWriter w;
  w.useExtension("KHR_materials_specular");
  const int tex = w.texture("cutout_checker.png");
  const int checker = w.material({{"name", "checker"},
                                  {"pbrMetallicRoughness",
                                   {{"baseColorTexture", {{"index", tex}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.8}}},
                                  {"alphaMode", "MASK"},
                                  {"alphaCutoff", 0.5},
                                  {"doubleSided", true}});
  const int back = w.material(lambert("back", {0.2f, 0.35f, 0.6f}));
  const int red = w.material(lambert("red", {0.7f, 0.15f, 0.1f}));
  const int green = w.material(lambert("green", {0.15f, 0.6f, 0.2f}));

  MeshData quad;
  addQuad(quad, {-1.2f, -0.7f, 0}, {1.2f, -0.7f, 0}, {1.2f, 0.7f, 0}, {-1.2f, 0.7f, 0}, {0, 0, 5}, 1);
  quad.uvs = {0, 1, 1, 1, 1, 0, 0, 0};
  w.node({{"name", "checker"}, {"mesh", w.mesh("checker", quad, checker)}, {"translation", {0, 0, 0.6}}});

  MeshData wall;  // 2 triangles: a triangle-list BLAS
  addQuad(wall, {-3, -2, 0}, {3, -2, 0}, {3, 2, 0}, {-3, 2, 0}, {0, 0, 5}, 1);
  w.node({{"name", "wall"}, {"mesh", w.mesh("wall", wall, back)}, {"translation", {0, 0, -1.0}}});

  MeshData tri;  // 1 triangle
  tri.positions = {{-0.3f, -0.25f, 0}, {0.3f, -0.25f, 0}, {0, 0.3f, 0}};
  tri.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  tri.indices = {0, 1, 2};
  w.node({{"name", "triangle"}, {"mesh", w.mesh("triangle", tri, green)}, {"translation", {1.15f, 0.45f, -0.5}}});

  const int sphere = w.mesh("sphere", uvSphere(0.25f, 24, 12), red);
  const json placements[] = {
      {{"translation", {-0.9, 0.3, -0.3}}},
      {{"translation", {-0.3, -0.3, -0.2}}, {"scale", {1.6, 0.6, 1.0}}},
      {{"translation", {0.35, 0.25, -0.4}}, {"rotation", {0.2588190, 0, 0.2588190, 0.9305076}}, {"scale", {0.7, 1.8, 0.7}}},
      {{"translation", {0.9, -0.35, -0.1}}, {"scale", {0.5, 0.5, 0.5}}},
      {{"translation", {0.0, 0.0, 0.2}}, {"scale", {0.3, 0.3, 0.3}}}};
  int k = 0;
  for (const json& p : placements) {
    json n = p;
    n["name"] = "sphere" + std::to_string(k++);
    n["mesh"] = sphere;
    w.node(n);
  }

  const int cam = w.camera(
      {{"type", "orthographic"}, {"orthographic", {{"xmag", 1.6}, {"ymag", 0.9}, {"znear", 0.01}, {"zfar", 100.0}}}});
  w.node({{"name", "Camera"}, {"camera", cam}, {"translation", {0, 0, 4.0}}});
  w.write(dir / "cutout.gltf");
  writeJson(dir / "cutout.pen.json",
            {{"scene", "cutout.gltf"}, {"environment", {{"color", {1.0, 1.0, 1.0}}}}, {"render", {{"res", "320x180"}}}});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: penumbra-scenegen <output folder>\n");
    return 1;
  }
  const fs::path dir = argv[1];
  fs::create_directories(dir);
  cornell(dir);
  sphereScene(dir, "furnace_diffuse", lambert("white", {1, 1, 1}), {{"res", "64x64"}});
  sphereScene(dir, "furnace_glass",
              {{"name", "glass"},
               {"pbrMetallicRoughness", {{"baseColorFactor", {1, 1, 1, 1}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.0}}},
               {"extensions", {{"KHR_materials_transmission", {{"transmissionFactor", 1.0}}}, {"KHR_materials_ior", {{"ior", 1.5}}}}}},
              {{"res", "64x64"}, {"max_bounces", 128}, {"no_rr", true}});
  furnaceFog(dir);
  spheres(dir);
  bigtex(dir);
  cutout(dir);
  std::printf("wrote test scenes to %s\n", dir.string().c_str());
  return 0;
}
