#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/package/package.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen::ui {

// A scene chosen on the Render page, loaded and packaged in the background (docs/GUIDE.md "UI": "Missing files are
// listed here, before any render"). Packaging is the same check a render does first.
struct SceneInfo {
  RenderSettings settings;  // resolved like the command line: config file, then the .pen.json
  std::shared_ptr<const Scene> scene;
  SceneStats stats;
  uint32_t lights = 0;  // emissive meshes count as one each, plus punctual lights and the environment
  std::vector<std::string> cameras;
  uint64_t package_bytes = 0;
  size_t package_files = 0;
  std::string error;  // why it cannot render (missing file, bad scene), empty when fine
  std::vector<std::string> warnings;
};

// Loads `path` (.glb, .gltf, .obj or .pen.json) like `penumbra <path>` would: the user config file, then the
// .pen.json. Never throws: problems are in `error`.
SceneInfo inspectScene(const std::filesystem::path& path);

// A small preview of the framing: `width` x `height`, a few samples on this machine's CPU, RGBA8 rows top-down
// through the same display transform as the output. Never throws (empty on failure).
std::vector<uint8_t> renderPreview(const Scene& scene, const RenderSettings& settings, uint32_t width,
                                   uint32_t height, uint32_t spp);

}  // namespace pen::ui
