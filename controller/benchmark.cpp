#include "controller/benchmark.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <vector>

#include "controller/hosting/hosting_session.h"
#include "controller/render_job.h"
#include "core/config/config.h"
#include "core/exit_codes.h"
#include "core/settings/schema.h"
#include "core/system/paths.h"
#include "core/version.h"
#include "worker/device_manager.h"

namespace pen {

namespace {

// The repo suite, light to heavy (scripts/get-scenes downloads the Sponzas).
constexpr const char* kSuite[] = {"cornell", "spheres", "sponza", "intel-sponza"};

struct Row {
  std::string scene, device;
  BackendKind backend = BackendKind::Cpu;
  uint64_t samples = 0;
  uint32_t items = 0;
  double device_rate = 0, window_rate = 0, busy = 0;  // M samples/s, M samples/s, percent
  double ready_seconds = 0, window_seconds = 0;
  std::string placement;
  bool failed = false;
};

Row rowFor(const std::string& scene, const DeviceTally& t) {
  Row row;
  row.scene = scene;
  // "local/0 NVIDIA GeForce RTX 4060 Ti": the device name without the worker and slot number.
  const size_t space = t.name.find(' ');
  row.device = space == std::string::npos ? t.name : t.name.substr(space + 1);
  row.backend = t.backend;
  row.samples = t.samples;
  row.items = t.items;
  row.ready_seconds = t.ready_seconds;
  row.placement = t.placement;
  row.failed = t.failed;
  row.window_seconds = t.first_begin >= 0 ? t.last_end - t.first_begin : 0;
  if (t.seconds > 0) row.device_rate = t.samples / 1e6 / t.seconds;
  if (row.window_seconds > 0) {
    row.window_rate = t.samples / 1e6 / row.window_seconds;
    row.busy = std::min(100.0, 100 * t.seconds / row.window_seconds);
  }
  return row;
}

std::string markdown(const std::vector<Row>& rows, const RenderSettings& s) {
  std::string md = fmt::format("{}, {}x{}, {}\n\n", versionString(), s.outputWidth(), s.outputHeight(),
                               s.spp ? fmt::format("{} spp", s.spp) : fmt::format("{} s per run", s.time_limit_s));
  md += "| Device | Backend | Scene | M samples/s (rendering) | M samples/s (window) | Busy % | Upload + build (s) | "
        "Placement |\n| --- | --- | --- | --- | --- | --- | --- | --- |\n";
  for (const Row& r : rows)
    md += fmt::format("| {} | {} | {} | {} | {:.2f} | {:.0f} | {:.1f} | {} |\n", r.device, toString(r.backend), r.scene,
                      r.failed ? std::string("failed") : fmt::format("{:.2f}", r.device_rate), r.window_rate, r.busy,
                      r.ready_seconds, r.placement);
  return md;
}

nlohmann::json toJson(const std::vector<Row>& rows, const RenderSettings& s) {
  nlohmann::json j;
  j["version"] = std::string(appVersion());
  j["git"] = std::string(gitHash());
  j["width"] = s.outputWidth();
  j["height"] = s.outputHeight();
  j["spp"] = s.spp;
  j["time_limit_s"] = s.time_limit_s;
  j["results"] = nlohmann::json::array();
  for (const Row& r : rows)
    j["results"].push_back({{"scene", r.scene},
                            {"device", r.device},
                            {"backend", std::string(toString(r.backend))},
                            {"failed", r.failed},
                            {"samples", r.samples},
                            {"items", r.items},
                            {"m_samples_per_s_rendering", r.device_rate},
                            {"m_samples_per_s_window", r.window_rate},
                            {"busy_percent", r.busy},
                            {"window_seconds", r.window_seconds},
                            {"ready_seconds", r.ready_seconds},
                            {"placement", r.placement}});
  return j;
}

}  // namespace

int runBenchmark(const BenchmarkOptions& options) {
  // The scenes: the one given, else the suite in ./scenes.
  std::vector<std::pair<std::string, std::filesystem::path>> scenes;
  if (options.cli.contains("scene")) {
    const std::filesystem::path p = pathFromUtf8(options.cli["scene"].get<std::string>());
    scenes.emplace_back(pathToUtf8(p.stem().stem()), p);
  } else {
    for (const char* name : kSuite) {
      const std::filesystem::path p = std::filesystem::path("scenes") / (std::string(name) + ".pen.json");
      std::error_code ec;
      if (std::filesystem::is_regular_file(p, ec)) scenes.emplace_back(name, p);
    }
    if (scenes.empty()) {
      spdlog::error("no scenes in ./scenes: run the benchmark from the Penumbra folder, or give a scene "
                    "(penumbra --benchmark scene.glb)");
      return toInt(ExitCode::BadArguments);
    }
  }

  const std::filesystem::path temp = std::filesystem::temp_directory_path() / "penumbra-benchmark";
  std::vector<Row> rows;
  RenderSettings last;
  for (const auto& [name, path] : scenes) {
    SettingsSources sources;
    sources.cli = options.cli;
    sources.config_file = options.config_file;
    sources.load_user_config = options.load_user_config;
    sources.cli["scene"] = pathToUtf8(path);
    if (!sources.cli.contains("res")) sources.cli["res"] = "1080p";  // one size for every scene
    if (!sources.cli.contains("spp")) {
      sources.cli["spp"] = "0";  // until the time limit
      if (!sources.cli.contains("time_limit")) sources.cli["time_limit"] = "30s";
    }
    sources.cli["output"] = pathToUtf8(temp / (name + ".exr"));
    RenderSettings s;
    try {
      s = resolveSettings(sources).settings;
    } catch (const SettingsError& e) {
      // The Sponzas need scripts/get-scenes first.
      spdlog::info("benchmark: skipping {}: {}", name, e.what());
      continue;
    }
    last = s;

    spdlog::info("benchmark: loading {}", name);
    RenderJob job;
    if (const int rc = prepareNewRender(s, job); rc >= 0) {
      spdlog::error("benchmark: {} could not be prepared", name);
      continue;
    }
    const FilmLayout layout = job.film->layout();
    const FilmIdentity identity = job.film->identity();
    const std::filesystem::path filmPath = job.film->path();
    const bool inRam = job.film->inRam();

    size_t deviceCount = 0;
    try {
      deviceCount = createDevices(s, /*controllerHost=*/true).devices.size();
    } catch (const std::exception& e) {
      spdlog::error("{}", e.what());
      return toInt(ExitCode::NoUsableDevice);
    }
    // Each device alone: the same scene, a new film per run.
    for (size_t d = 0; d < deviceCount; ++d) {
      DeviceSelection all = createDevices(s, true);
      std::vector<std::unique_ptr<IBackend>> one;
      one.push_back(std::move(all.devices[d]));
      all.devices.clear();  // the other devices are released before this one renders
      const std::string device = one.front()->info().name;
      if (d > 0) {
        job.film->close();
        job.film = Film::create(filmPath, layout, identity, inRam);
      }
      spdlog::info("benchmark: {} on {}", name, device);
      HostingSession session;
      RenderOutcome outcome;
      try {
        outcome = session.render(job, std::move(one), RenderRunOptions{});
      } catch (const std::exception& e) {
        spdlog::error("benchmark: {} on {} failed: {}", name, device, e.what());
        continue;
      }
      for (const DeviceTally& t : outcome.devices) {
        rows.push_back(rowFor(name, t));
        const Row& r = rows.back();
        spdlog::info("benchmark: {} on {} ({}): {:.2f} M samples/s rendering, {:.2f} over {:.1f}s, busy {:.0f}%, "
                     "upload + build {:.1f}s",
                     name, r.device, toString(r.backend), r.device_rate, r.window_rate, r.window_seconds, r.busy,
                     r.ready_seconds);
      }
    }
    job.film->close();
  }
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  if (rows.empty()) {
    spdlog::error("benchmark: nothing was rendered");
    return toInt(ExitCode::NoUsableDevice);
  }

  const std::string md = markdown(rows, last);
  std::fputs(("\n" + md).c_str(), stdout);
  std::filesystem::path mdPath = options.output;
  mdPath.replace_extension(".md");
  if (!options.output.parent_path().empty()) std::filesystem::create_directories(options.output.parent_path(), ec);
  std::ofstream json(options.output), table(mdPath);
  json << toJson(rows, last).dump(2) << "\n";
  table << md;
  json.close();
  table.close();
  if (!json || !table) {
    spdlog::error("benchmark: cannot write {} or {}", pathToUtf8(options.output), pathToUtf8(mdPath));
    return toInt(ExitCode::OutOfDisk);
  }
  spdlog::info("benchmark: wrote {} and {}", pathToUtf8(options.output), pathToUtf8(mdPath));
  return toInt(ExitCode::Success);
}

}  // namespace pen
