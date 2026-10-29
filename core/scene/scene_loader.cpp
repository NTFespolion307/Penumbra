#include "core/scene/scene_loader.h"

#include <fmt/format.h>

#include "core/image/image_io.h"
#include "core/scene/sky.h"
#include "core/settings/parse.h"
#include "core/system/paths.h"

namespace pen {

Medium buildMedium(const RenderSettings& settings) {
  Medium m;
  const AtmosphereSettings& a = settings.atmosphere;
  if (!a.enabled) return m;
  const float mpu = static_cast<float>(a.meters_per_unit);
  // Earth's air and haze at sea level, per metre, as in Blender's Nishita sky (Rayleigh for 680/550/440 nm; Mie
  // 2e-5 scattering with 11% absorption), with their scale heights. y = 0 is `altitude` metres above sea level.
  const auto earth = [&](Vec3f scatter, float absorbFactor, float heightM, float g, bool rayleigh, double amount) {
    if (amount <= 0) return;
    MediumLayer& l = m.layers[m.count++];
    l.sigma_s = scatter * (static_cast<float>(amount) * mpu);
    l.sigma_t = l.sigma_s * absorbFactor;
    l.base = static_cast<float>(-a.altitude) / mpu;
    l.inv_height = mpu / heightM;
    l.g = g;
    l.rayleigh = rayleigh;
  };
  const SkySettings& sky = settings.sky;
  earth({5.802e-6f, 13.558e-6f, 33.1e-6f}, 1.0f, 8000, 0, true, a.air.value_or(sky.enabled ? sky.air : 1.0));
  earth(Vec3f(2e-5f), 1.11f, 1200, static_cast<float>(a.haze_anisotropy), false,
        a.haze.value_or(sky.enabled ? sky.dust : 1.0));
  if (a.fog_density > 0) {
    MediumLayer& l = m.layers[m.count++];
    l.sigma_t = Vec3f(static_cast<float>(a.fog_density) * mpu);
    l.sigma_s = l.sigma_t * Vec3f(static_cast<float>(a.fog_color[0]), static_cast<float>(a.fog_color[1]),
                                  static_cast<float>(a.fog_color[2]));
    l.base = static_cast<float>(a.fog_base);
    l.inv_height = mpu / static_cast<float>(a.fog_height);
    l.g = static_cast<float>(a.fog_anisotropy);
  }
  return m;
}

Scene loadScene(const RenderSettings& settings, std::vector<std::string>& warnings) {
  if (settings.scene.empty()) throw SceneError("no scene given");
  const std::string ext = toLower(pathToUtf8(settings.scene.extension()));
  Scene scene = ext == ".obj" ? loadObj(settings.scene, warnings) : loadGltf(settings.scene, warnings);
  if (scene.instances.empty()) warnings.push_back("the scene contains no meshes");

  applyEnvironment(scene, settings, warnings);
  return scene;
}

void applyEnvironment(Scene& scene, const RenderSettings& settings, std::vector<std::string>& warnings) {
  const EnvironmentSettings& env = settings.environment;
  scene.environment = Environment{};
  if (settings.sky.enabled) {
    // The physical sky is the environment (D-120): baked here, then sampled like an HDRI on every backend.
    if (!env.hdri.empty()) warnings.push_back("\"sky\" replaces the environment HDRI");
    scene.environment.map = bakeSky(settings.sky);
    scene.environment.name = fmt::format("physical sky (sun {:.1f} deg up, {:.1f} deg azimuth)",
                                         settings.sky.sun_elevation_deg, settings.sky.sun_azimuth_deg);
    scene.medium = buildMedium(settings);
    scene.finalize();
    return;
  }
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
  scene.medium = buildMedium(settings);
  scene.finalize();
}

}  // namespace pen
