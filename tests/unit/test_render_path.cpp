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

TEST_CASE("scheduler covers every pixel once, centre first", "[scheduler]") {
  Scheduler s(2560, 1440, 256, 0, 64, std::nullopt);
  std::vector<int> cover(2560 * 1440, 0);
  std::optional<WorkItem> first;
  while (auto item = s.next()) {
    if (!first) first = item;
    for (uint32_t y = item->rect.y0; y < item->rect.y1; ++y)
      for (uint32_t x = item->rect.x0; x < item->rect.x1; ++x) ++cover[y * 2560 + x];
    CHECK(item->sample_begin == 0);
    CHECK(item->sample_end == 64);
  }
  CHECK(std::all_of(cover.begin(), cover.end(), [](int c) { return c == 1; }));
  REQUIRE(first);
  CHECK(first->rect.x0 <= 1280);
  CHECK(first->rect.x1 >= 1280);
  CHECK(s.total() == 10 * 6);
  CHECK(s.outstanding() == 60);
  CHECK(s.complete(0));
  CHECK_FALSE(s.complete(0));  // duplicate
}

TEST_CASE("frames below 1080p are one item; regions clip tiles", "[scheduler]") {
  CHECK(filmTileSize(1280, 720, 256) == 1280);
  CHECK(filmTileSize(1920, 1080, 256) == 256);
  Scheduler small(1280, 720, filmTileSize(1280, 720, 256), 0, 16, std::nullopt);
  CHECK(small.total() == 1);
  CHECK(small.next()->rect == TileRect{0, 0, 1280, 720});

  Scheduler region(3840, 2160, 256, 0, 16, PixelRect{100, 100, 600, 300});
  uint64_t pixels = 0;
  while (auto item = region.next()) {
    CHECK(item->rect.x0 >= 100);
    CHECK(item->rect.y1 <= 300);
    pixels += item->rect.pixels();
  }
  CHECK(pixels == 500 * 200);
}

TEST_CASE("scheduler skips tiles already in the film (resume)", "[scheduler]") {
  Scheduler s(3840, 2160, 256, 0, 8, std::nullopt);
  const uint64_t skipped = s.skipDone([](uint32_t tile) { return tile % 2 == 0; });
  CHECK(s.completed() > 0);
  CHECK(skipped > 0);
  uint32_t issued = 0;
  while (auto item = s.next()) {
    CHECK(item->tile % 2 == 1);
    ++issued;
  }
  CHECK(issued + s.completed() == s.total());
}

TEST_CASE("scheduler requeues released items", "[scheduler]") {
  Scheduler s(3840, 2160, 256, 0, 8, std::nullopt);
  const WorkItem a = *s.next();
  s.requeue(a.id);
  CHECK(s.next()->id == a.id);
}

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
