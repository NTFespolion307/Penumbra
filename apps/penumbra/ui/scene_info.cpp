#include "apps/penumbra/ui/scene_info.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "backends/cpu/cpu_backend.h"
#include "core/config/config.h"
#include "core/output/output.h"
#include "core/render/camera.h"
#include "core/render/integrator.h"
#include "core/scene/scene_loader.h"
#include "core/settings/schema.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"

namespace pen::ui {

SceneInfo inspectScene(const std::filesystem::path& path) {
  SceneInfo info;
  try {
    SettingsSources src;
    src.cli["scene"] = pathToUtf8(path);
    ResolvedSettings r = resolveSettings(src);
    info.settings = r.settings;
    info.warnings = r.warnings;
    const ScenePackage pkg = buildPackage(info.settings);  // every file found and hashed, or SceneError
    info.package_bytes = pkg.total_bytes;
    info.package_files = pkg.files.size();
    auto scene = std::make_shared<Scene>(loadScene(info.settings, info.warnings));
    info.stats = scene->stats();
    info.lights = scene->lightCount();
    for (const Camera& c : scene->cameras) info.cameras.push_back(c.name.empty() ? "(unnamed camera)" : c.name);
    info.scene = std::move(scene);
  } catch (const std::exception& e) {
    info.error = e.what();
  }
  return info;
}

std::vector<uint8_t> renderPreview(const Scene& loaded, const RenderSettings& loadedWith, const RenderSettings& settings,
                                   uint32_t width, uint32_t height, uint32_t spp) {
  try {
    const Scene* sceneP = &loaded;
    std::unique_ptr<Scene> changed;
    if (settings.sky != loadedWith.sky || settings.atmosphere != loadedWith.atmosphere ||
        settings.environment != loadedWith.environment) {
      changed = std::make_unique<Scene>(loaded);
      std::vector<std::string> ignored;
      applyEnvironment(*changed, settings, ignored);
      sceneP = changed.get();
    }
    const Scene& scene = *sceneP;
    std::vector<std::string> notes;
    FrameSetup frame;
    frame.camera = RenderCamera(scene, settings, width, height, notes);
    frame.params = IntegratorParams::from(settings, scene);
    frame.seed = settings.seed;
    // A UI preview on this machine's CPU, leaving two threads for the UI (docs/GUIDE.md "Render page").
    const uint32_t threads = std::max(1u, usableCpuThreads() > 2 ? usableCpuThreads() - 2 : 1u);
    auto cpu = createCpuBackend(threads);
    cpu->upload(scene, Placement{});
    cpu->prepare(frame);
    WorkItem item;
    item.rect = {0, 0, width, height};
    item.sample_begin = 0;
    item.sample_end = spp;
    TileResult out;
    cpu->render(item, out);
    const float scale = static_cast<float>(std::exp2(settings.exposure));
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    for (size_t i = 0; i < out.mean.size(); ++i) {
      for (int c = 0; c < 3; ++c)
        rgba[i * 4 + c] = static_cast<uint8_t>(std::lround(255.0f * displayEncode(out.mean[i][c], scale)));
      rgba[i * 4 + 3] = 255;
    }
    return rgba;
  } catch (const std::exception& e) {
    spdlog::warn("UI: framing preview failed: {}", e.what());
    return {};
  }
}

}  // namespace pen::ui
