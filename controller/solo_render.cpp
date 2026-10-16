#include "controller/solo_render.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <fmt/format.h>

#include "controller/local_worker.h"
#include "core/exit_codes.h"
#include "core/output/output.h"
#include "core/render/camera.h"
#include "core/scene/scene_loader.h"
#include "core/settings/parse.h"
#include "core/system/paths.h"
#include "core/version.h"
#include "worker/device_manager.h"

namespace pen {

int runLocalRender(const RenderSettings& s, const std::function<void(const RenderProgress&)>& progress) {
  if (s.integrator == Integrator::DebugBvh) {
    spdlog::error("--integrator debug:bvh needs a GPU backend (Vulkan RT M5, CUDA M6); Embree does not report "
                  "traversal steps");
    return toInt(ExitCode::BadArguments);
  }
  if (s.format != ImageFormat::Png && s.format != ImageFormat::Exr) {
    spdlog::error("{} output is built in milestone M2; use .png or .exr", toString(*s.format));
    return toInt(ExitCode::BadArguments);
  }
  if (s.spp == 0) {
    spdlog::error("--spp 0 (render until the time limit) needs progressive passes, built in milestone M3");
    return toInt(ExitCode::BadArguments);
  }
  if (s.denoise != Denoiser::None) spdlog::warn("--denoise {} is built in milestone M11; not applied", toString(s.denoise));
  if (!s.aov.empty()) spdlog::warn("--aov is built in milestone M11; no extra passes are written");

  // 1. Scene.
  auto scene = std::make_shared<Scene>();
  try {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> warnings;
    *scene = loadScene(s, warnings);
    for (const auto& w : warnings) spdlog::warn("scene: {}", w);
    const SceneStats st = scene->stats();
    spdlog::info("scene {}: {} triangles ({} unique), {} instances, {} materials, {} textures ({}), {} emissive "
                 "triangles, {} punctual lights, environment {}; loaded in {:.2f}s",
                 pathToUtf8(s.scene.filename()), st.triangles, st.unique_triangles, st.instances, st.materials,
                 st.textures, formatBytes(st.texture_bytes), st.emissive_triangles, st.punctual_lights,
                 scene->environment.hasMap() ? scene->environment.name
                                             : (scene->environment.isBlack() ? "none" : "uniform colour"),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (scene->lightCount() == 0) spdlog::warn("the scene has no lights and no environment; the image will be black");
    std::vector<std::string> notes;
    RenderCamera(*scene, s, s.outputWidth(), s.outputHeight(), notes);  // validates --camera early
    for (const auto& n : notes) spdlog::info("{}", n);
  } catch (const SceneError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::SceneLoadError);
  }

  // 2. Devices.
  DeviceSelection devices;
  try {
    devices = createDevices(s, /*controllerHost=*/true);
  } catch (const NoDeviceError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  } catch (const BackendError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  }
  for (const auto& n : devices.notes) spdlog::info("{}", n);
  for (size_t i = 0; i < devices.devices.size(); ++i) {
    const DeviceInfo d = devices.devices[i]->info();
    spdlog::info("device {}: {} (backend {}, {} threads)", i, d.name, toString(d.backend), d.threads);
  }

  // 3. Render through controller -> scheduler -> WorkerLink -> film.
  RenderController controller(s, scene);
  controller.addWorker(std::make_unique<LocalWorker>(controller.nextWorkerId(), controller, std::move(devices.devices)));
  RenderOutcome outcome;
  try {
    outcome = controller.run(progress);
  } catch (const std::exception& e) {
    spdlog::error("render failed: {}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  }

  // 4. Output.
  const auto now = std::time(nullptr);
  char date[32];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  std::string deviceNames;
  for (const auto& d : outcome.devices) deviceNames += (deviceNames.empty() ? "" : "; ") + d.name;
  const ImageMetadata metadata = {
      {"Software", versionString()},
      {"Scene", pathToUtf8(s.scene)},
      {"Resolution", fmt::format("{}x{}", s.outputWidth(), s.outputHeight())},
      {"Samples", std::to_string(s.spp)},
      {"MaxBounces", std::to_string(s.max_bounces)},
      {"Integrator", std::string(toString(s.integrator))},
      {"Seed", std::to_string(s.seed)},
      {"Backend", std::string(toString(s.backend))},
      {"Devices", deviceNames},
      {"RenderTime", fmt::format("{:.2f}s", outcome.seconds)},
      {"Date", date},
  };
  try {
    std::vector<std::string> notes;
    writeOutput(controller.film(), s, metadata, notes);
    for (const auto& n : notes) spdlog::info("{}", n);
  } catch (const ImageIoError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::OutOfDisk);
  }

  // 5. Summary.
  uint64_t samples = 0;
  for (const auto& d : outcome.devices) samples += d.samples;
  spdlog::info("done in {:.2f}s: {:.2f} M samples, {:.2f} M samples/s; wrote {}", outcome.seconds, samples / 1e6,
               outcome.seconds > 0 ? samples / 1e6 / outcome.seconds : 0.0, pathToUtf8(s.output));
  for (const auto& d : outcome.devices)
    spdlog::info("  {}: {} tiles, {:.2f} M samples, {:.2f}s", d.name, d.tiles, d.samples / 1e6, d.seconds);
  if (outcome.nan_count) spdlog::warn("{} non-finite samples were replaced by 0", outcome.nan_count);
  if (outcome.time_limited && !outcome.complete) {
    spdlog::warn("stopped by --time-limit before every tile was rendered; unrendered tiles are black");
  }
  return toInt(ExitCode::Success);
}

}  // namespace pen
