#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <vector>

#include "controller/scheduler.h"
#include "core/film/film.h"
#include "core/scene/scene_loader.h"

using namespace pen;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("penumbra-gltf-" + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// One triangle (positions only) as base64 data: (0,0,0) (1,0,0) (0,1,0).
constexpr const char* kTriangleGltf = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength", "KHR_materials_volume"],
  "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point", "color": [1, 1, 1], "intensity": 683}]}},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2, 3]}],
  "nodes": [
    {"name": "tri", "mesh": 0},
    {"name": "tri moved", "mesh": 0, "translation": [0, 0, -2], "scale": [-1, 1, 1]},
    {"name": "Cam", "camera": 0, "translation": [0, 0, 5]},
    {"name": "Lamp", "translation": [0, 3, 0], "extensions": {"KHR_lights_punctual": {"light": 0}}}
  ],
  "cameras": [{"name": "CamData", "type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.1}}],
  "materials": [{"name": "glow", "emissiveFactor": [1, 0.5, 0.25],
                 "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36,
               "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]
})";

}  // namespace

TEST_CASE("glTF loader: meshes, instances, cameras, lights, emission", "[gltf]") {
  TempDir dir;
  std::ofstream(dir.path / "tri.gltf") << kTriangleGltf;
  std::vector<std::string> warnings;
  Scene scene = loadGltf(dir.path / "tri.gltf", warnings);
  scene.finalize();
  CHECK(scene.meshes.size() == 1);
  CHECK(scene.instances.size() == 2);
  CHECK(scene.meshes[0].primitives[0].triangleCount() == 1);
  REQUIRE(scene.cameras.size() == 2);  // node name, then the camera data name as an alias
  CHECK(scene.cameras[0].name == "Cam");
  CHECK(scene.cameras[1].name == "CamData");
  REQUIRE(scene.lights.size() == 1);
  CHECK(std::fabs(scene.lights[0].intensity.x - 1.0f) < 1e-6f);  // 683 cd -> 1 W/sr
  CHECK(std::fabs(scene.lights[0].position.y - 3) < 1e-6f);
  CHECK(scene.emissive_triangles.size() == 2);
  CHECK(scene.materials[0].emissive_strength == 4);
  // Front face is +Z for the original; the mirrored instance (negative determinant) keeps glTF's rule.
  CHECK(scene.emissive_triangles[0].normal.z > 0.99f);
  CHECK(scene.emissive_triangles[1].normal.z > 0.99f);
  // KHR_materials_volume is not supported and must be reported.
  bool warned = false;
  for (const auto& w : warnings) warned |= w.find("KHR_materials_volume") != std::string::npos;
  CHECK(warned);
  CHECK(scene.lightCount() == 3);  // 2 emissive triangles + 1 point light, no environment
}
