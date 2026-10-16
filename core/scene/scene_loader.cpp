#include "core/scene/scene_loader.h"

#include <fmt/format.h>

#include "core/image/image_io.h"
#include "core/settings/parse.h"
#include "core/system/paths.h"

namespace pen {

Scene loadScene(const RenderSettings& settings, std::vector<std::string>& warnings) {
  if (settings.scene.empty()) throw SceneError("no scene given");
  const std::string ext = toLower(pathToUtf8(settings.scene.extension()));
  Scene scene = ext == ".obj" ? loadObj(settings.scene, warnings) : loadGltf(settings.scene, warnings);
  if (scene.instances.empty()) warnings.push_back("the scene contains no meshes");

  const EnvironmentSettings& env = settings.environment;
  scene.environment.strength = static_cast<float>(env.strength);
  scene.environment.rotation = static_cast<float>(env.rotation_deg) * kPi / 180.0f;
  if (!env.hdri.empty()) {
    try {
      scene.environment.map = loadImageRGB(env.hdri);
      scene.environment.name = pathToUtf8(env.hdri.filename());
    } catch (const ImageIoError& e) {
      throw SceneError(fmt::format("environment: {}", e.what()));
    }
  } else {
    scene.environment.color = Vec3f(static_cast<float>(env.color[0]), static_cast<float>(env.color[1]),
                                    static_cast<float>(env.color[2])) *
                              static_cast<float>(env.strength);
  }
  scene.finalize();
  return scene;
}

}  // namespace pen
