#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

// Scene load failure; maps to exit code 2.
struct SceneError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Loads the scene file named in the settings (.glb, .gltf or .obj), the environment from the .pen.json, and
// finalizes light sampling. Non-fatal problems (unsupported extensions, skipped primitives) go to `warnings`.
Scene loadScene(const RenderSettings& settings, std::vector<std::string>& warnings);

Scene loadGltf(const std::filesystem::path& path, std::vector<std::string>& warnings);
Scene loadObj(const std::filesystem::path& path, std::vector<std::string>& warnings);

// glTF extensions this build understands; anything else in extensionsUsed produces a warning.
bool isSupportedGltfExtension(const std::string& name);

}  // namespace pen
