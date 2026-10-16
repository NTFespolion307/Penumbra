// Wavefront OBJ loader for test meshes (secondary input). MTL: Kd -> base colour, Ke -> emission,
// Ns -> roughness, d -> alpha, map_Kd -> base colour texture. Everything else uses glTF defaults (dielectric).

#include <tiny_obj_loader.h>

#include <cmath>
#include <fmt/format.h>
#include <map>

#include "core/image/image_io.h"
#include "core/scene/scene_loader.h"
#include "core/system/paths.h"

namespace pen {

Scene loadObj(const std::filesystem::path& path, std::vector<std::string>& warnings) {
  tinyobj::ObjReaderConfig config;
  config.triangulate = true;
  config.mtl_search_path = pathToUtf8(path.parent_path());
  tinyobj::ObjReader reader;
  if (!reader.ParseFromFile(pathToUtf8(path), config))
    throw SceneError(fmt::format("cannot load OBJ '{}': {}", pathToUtf8(path), reader.Error()));
  if (!reader.Warning().empty()) warnings.push_back(fmt::format("OBJ: {}", reader.Warning()));

  const auto& attrib = reader.GetAttrib();
  Scene scene;
  std::map<std::string, int32_t> textures;
  for (const auto& om : reader.GetMaterials()) {
    Material m;
    m.name = om.name;
    m.base_color = {om.diffuse[0], om.diffuse[1], om.diffuse[2], om.dissolve};
    m.metallic = 0;
    m.roughness = std::sqrt(2.0f / (std::fmax(om.shininess, 0.0f) + 2.0f));  // Phong exponent -> roughness
    m.emissive = {om.emission[0], om.emission[1], om.emission[2]};
    const float emax = maxComponent(m.emissive);
    if (emax > 1) {
      m.emissive = m.emissive / emax;
      m.emissive_strength = emax;
    }
    if (om.dissolve < 1) m.alpha_mode = AlphaMode::Blend;
    if (!om.diffuse_texname.empty()) {
      auto it = textures.find(om.diffuse_texname);
      if (it == textures.end()) {
        int32_t index = -1;
        try {
          scene.textures.push_back(loadTextureFile(path.parent_path() / pathFromUtf8(om.diffuse_texname)));
          index = static_cast<int32_t>(scene.textures.size() - 1);
        } catch (const ImageIoError& e) {
          warnings.push_back(e.what());
        }
        it = textures.emplace(om.diffuse_texname, index).first;
      }
      m.base_color_tex.index = it->second;
    }
    scene.materials.push_back(m);
  }
  const uint32_t defaultMat = static_cast<uint32_t>(scene.materials.size());
  Material def;
  def.name = "default";
  def.metallic = 0;
  def.roughness = 0.5f;
  scene.materials.push_back(def);

  Mesh mesh;
  mesh.name = pathToUtf8(path.stem());
  for (const auto& shape : reader.GetShapes()) {
    // One primitive per material used by the shape.
    std::map<int, MeshPrimitive> byMaterial;
    const auto& idx = shape.mesh.indices;
    for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
      const int matId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[f];
      MeshPrimitive& p = byMaterial[matId];
      for (int k = 0; k < 3; ++k) {
        const tinyobj::index_t& i = idx[f * 3 + k];
        p.positions.push_back({attrib.vertices[3 * i.vertex_index], attrib.vertices[3 * i.vertex_index + 1],
                               attrib.vertices[3 * i.vertex_index + 2]});
        if (i.normal_index >= 0)
          p.normals.push_back({attrib.normals[3 * i.normal_index], attrib.normals[3 * i.normal_index + 1],
                               attrib.normals[3 * i.normal_index + 2]});
        if (i.texcoord_index >= 0)  // OBJ v=0 is the bottom; glTF v=0 is the top
          p.uv0.push_back({attrib.texcoords[2 * i.texcoord_index], 1 - attrib.texcoords[2 * i.texcoord_index + 1]});
        p.indices.push_back(static_cast<uint32_t>(p.indices.size()));
      }
    }
    for (auto& [matId, p] : byMaterial) {
      p.material = matId >= 0 ? static_cast<uint32_t>(matId) : defaultMat;
      if (p.normals.size() != p.positions.size()) p.normals.clear();
      if (p.uv0.size() != p.positions.size()) p.uv0.clear();
      mesh.primitives.push_back(std::move(p));
    }
  }
  scene.meshes.push_back(std::move(mesh));
  scene.instances.push_back({pathToUtf8(path.stem()), 0, Affine3f::identity()});
  return scene;
}

}  // namespace pen
