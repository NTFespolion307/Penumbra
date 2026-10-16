#include "core/scene/scene.h"

#include <cmath>

namespace pen {

float Scene::boundingRadius() const {
  if (!bounds.valid()) return 1.0f;
  return std::fmax(0.5f * length(bounds.extent()), 1e-3f);
}

void Scene::finalize() {
  // World bounds.
  bounds = Bounds3f{};
  for (const Instance& inst : instances) {
    for (const MeshPrimitive& prim : meshes[inst.mesh].primitives)
      for (const Vec3f& p : prim.positions) bounds.extend(inst.to_world.point(p));
  }
  if (!bounds.valid()) bounds.extend(Vec3f(0));

  // Emissive triangles in world space.
  emissive_triangles.clear();
  emissive_offset.assign(instances.size(), {});
  for (uint32_t i = 0; i < instances.size(); ++i) {
    const Instance& inst = instances[i];
    const Mesh& mesh = meshes[inst.mesh];
    emissive_offset[i].assign(mesh.primitives.size(), -1);
    // glTF: a negative determinant flips the winding, so the front face is clockwise.
    const float frontSign = inst.to_world.determinant() < 0 ? -1.0f : 1.0f;
    for (uint32_t p = 0; p < mesh.primitives.size(); ++p) {
      const MeshPrimitive& prim = mesh.primitives[p];
      const Material& mat = materials[prim.material];
      if (!mat.emits()) continue;
      emissive_offset[i][p] = static_cast<int32_t>(emissive_triangles.size());
      for (uint32_t t = 0; t < prim.triangleCount(); ++t) {
        const Vec3f a = inst.to_world.point(prim.positions[prim.indices[t * 3]]);
        const Vec3f b = inst.to_world.point(prim.positions[prim.indices[t * 3 + 1]]);
        const Vec3f c = inst.to_world.point(prim.positions[prim.indices[t * 3 + 2]]);
        EmissiveTriangle tri;
        tri.p0 = a;
        tri.e1 = b - a;
        tri.e2 = c - a;
        const Vec3f n = cross(tri.e1, tri.e2);
        tri.area = 0.5f * length(n);
        tri.normal = normalize(n) * frontSign;
        tri.instance = i;
        tri.primitive = p;
        tri.triangle = t;
        tri.double_sided = mat.double_sided;
        emissive_triangles.push_back(tri);  // zero-area triangles stay so indices line up; their weight is 0
      }
    }
  }

  // Environment importance map: luminance x sin(theta) per pixel.
  if (environment.hasMap()) {
    const ImageRGB& m = environment.map;
    std::vector<float> w(size_t(m.width) * m.height);
    for (uint32_t y = 0; y < m.height; ++y) {
      const float sinTheta = std::sin(kPi * (y + 0.5f) / m.height);
      for (uint32_t x = 0; x < m.width; ++x)
        w[size_t(y) * m.width + x] = std::fmax(luminance(m.at(x, y)), 0.0f) * sinTheta;
    }
    environment.sampling = Distribution2D(w, m.width, m.height);
  }

  // One alias table over every light, weighted by estimated power (DECISIONS D-020).
  const float r = boundingRadius();
  std::vector<float> weights;
  weights.reserve(emissive_triangles.size() + lights.size() + 1);
  for (const EmissiveTriangle& tri : emissive_triangles) {
    const Material& mat =
        materials[meshes[instances[tri.instance].mesh].primitives[tri.primitive].material];
    weights.push_back(luminance(mat.emissive) * mat.emissive_strength * tri.area * kPi * (tri.double_sided ? 2 : 1));
  }
  for (const PunctualLight& l : lights) {
    const float lum = luminance(l.intensity);
    switch (l.type) {
      case LightType::Point: weights.push_back(4 * kPi * lum); break;
      case LightType::Spot: weights.push_back(2 * kPi * lum * (1 - 0.5f * (l.cos_inner + l.cos_outer))); break;
      case LightType::Directional: weights.push_back(kPi * r * r * lum); break;
    }
  }
  if (!environment.isBlack()) {
    float avg = luminance(environment.color);
    if (environment.hasMap()) {
      double sum = 0;
      for (const Vec3f& p : environment.map.pixels) sum += luminance(p);
      avg = static_cast<float>(sum / environment.map.pixels.size()) * environment.strength;
    }
    weights.push_back(4 * kPi * kPi * r * r * avg);
  }
  light_table = AliasTable(weights);
}

SceneStats Scene::stats() const {
  SceneStats s;
  s.meshes = static_cast<uint32_t>(meshes.size());
  s.instances = static_cast<uint32_t>(instances.size());
  s.materials = static_cast<uint32_t>(materials.size());
  s.textures = static_cast<uint32_t>(textures.size());
  s.punctual_lights = static_cast<uint32_t>(lights.size());
  s.emissive_triangles = emissive_triangles.size();
  for (const Mesh& m : meshes) {
    for (const MeshPrimitive& p : m.primitives) {
      s.unique_triangles += p.triangleCount();
      s.geometry_bytes += p.positions.size() * sizeof(Vec3f) + p.normals.size() * sizeof(Vec3f) +
                          p.tangents.size() * sizeof(Vec4f) + (p.uv0.size() + p.uv1.size()) * sizeof(Vec2f) +
                          p.colors.size() * sizeof(Vec4f) + p.indices.size() * sizeof(uint32_t);
    }
  }
  for (const Instance& inst : instances)
    for (const MeshPrimitive& p : meshes[inst.mesh].primitives) s.triangles += p.triangleCount();
  for (const Texture& t : textures) s.texture_bytes += t.bytes();
  s.texture_bytes += environment.map.pixels.size() * sizeof(Vec3f);
  return s;
}

}  // namespace pen
