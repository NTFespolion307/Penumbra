#include "controller/solo_render.h"

#include <spdlog/spdlog.h>

#include <fmt/format.h>

#include "controller/hosting/hosting_session.h"
#include "controller/render_job.h"
#include "core/exit_codes.h"
#include "core/film/film.h"
#include "core/output/output.h"
#include "core/output/writers.h"
#include "core/system/paths.h"
#include "core/version.h"
#include "net/join_code.h"
#include "worker/device_manager.h"

namespace pen {

namespace {

// The join code: --code / PENUMBRA_CODE, else the one saved in the user data folder (docs/GUIDE.md "Join code").
// Returns false (logged) when it cannot be had.
bool joinCode(const std::string& given, std::string& code) {
  if (!given.empty()) {
    const auto c = net::normalizeJoinCode(given);
    if (!c) {
      spdlog::error("--code '{}' is not a join code (16 characters like K7QD-M2XR-9FHT-4WNB)", given);
      return false;
    }
    code = *c;
    spdlog::info("join code {} (from --code)", net::formatJoinCode(code));
    return true;
  }
  const std::filesystem::path file = paths::userDataDir() / "join_code.txt";
  try {
    bool created = false;
    code = net::loadOrCreateJoinCode(file, created);
    spdlog::info("join code {} ({} {})", net::formatJoinCode(code), created ? "new, saved in" : "saved in",
                 pathToUtf8(file));
    return true;
  } catch (const std::exception& e) {
    spdlog::error("cannot create the join code in '{}': {}", pathToUtf8(file), e.what());
    return false;
  }
}

// Renders a prepared job through a hosting session (the same path the UI uses): the local devices, and with
// --host every worker that joins, then writes the output.
int renderAndFinish(RenderJob& job, const LocalRenderOptions& options) {
  const RenderSettings& s = job.settings;
  DeviceSelection devices;
  if (s.no_local) {
    spdlog::info("--no-local: this machine coordinates only; workers do all the rendering");
  } else {
    try {
      devices = createDevices(s, /*controllerHost=*/true);
    } catch (const NoDeviceError& e) {
      spdlog::error("{}", e.what());
      return toInt(ExitCode::NoUsableDevice);
    } catch (const BackendError& e) {
      spdlog::error("{}", e.what());
      return toInt(ExitCode::NoUsableDevice);
    }
  }
  for (const auto& n : devices.notes) spdlog::info("{}", n);
  for (size_t i = 0; i < devices.devices.size(); ++i) {
    const DeviceInfo d = devices.devices[i]->info();
    spdlog::info("device {}: {} (backend {}, {} threads)", i, d.name, toString(d.backend), d.threads);
  }

  HostingSession session;
  if (options.host) {
    std::string code;
    if (!joinCode(options.code, code)) return toInt(ExitCode::BadArguments);
    job.session.hosting = true;  // --resume hosts again with this code (docs/GUIDE.md "Controller crash")
    job.session.join_code = code;
    RenderProgress sofar;  // what the session file already says (a resumed render keeps its counts)
    sofar.items_done = job.session.tiles_done;
    sofar.items_total = job.film->layout().tileCount();
    sofar.samples_done = job.session.samples_done;
    sofar.samples_total = job.session.samples_total;
    saveSessionProgress(job, sofar);
    try {
      session.startHosting(s, code);
    } catch (const std::exception& e) {
      spdlog::error("{}", e.what());
      return toInt(ExitCode::BadArguments);
    }
    if (s.no_local) spdlog::info("waiting for workers to join");
  }

  RenderRunOptions run;
  run.interrupt = options.interrupt;
  run.progress = options.progress;
  run.after_checkpoint = [&](const RenderProgress& p) { saveSessionProgress(job, p); };
  RenderOutcome outcome;
  try {
    outcome = session.render(job, std::move(devices.devices), run);
  } catch (const std::exception& e) {
    spdlog::error("render failed: {}", e.what());
    session.stopHosting(std::chrono::seconds(0));
    return toInt(ExitCode::NoUsableDevice);
  }
  // Workers got RenderComplete; give them a moment to say Bye before closing their connections.
  session.stopHosting(std::chrono::seconds(5));
  return finishRender(job, outcome);
}

}  // namespace

int runLocalRender(const RenderSettings& s, const LocalRenderOptions& options) {
  RenderJob job;
  if (const int rc = prepareNewRender(s, job); rc >= 0) return rc;
  return renderAndFinish(job, options);
}

int resumeLocalRender(const std::filesystem::path& sessionFile, const nlohmann::json& cliOverrides,
                      const LocalRenderOptions& options) {
  RenderJob job;
  if (const int rc = prepareResumedRender(sessionFile, cliOverrides, job); rc >= 0) return rc;
  LocalRenderOptions opts = options;
  if (job.session.hosting) {
    // The render was hosting: host again with the same code, so --stay workers find it (docs/GUIDE.md).
    opts.host = true;
    if (opts.code.empty()) opts.code = job.session.join_code;
  }
  return renderAndFinish(job, opts);
}

int mergeFilmsToImage(const std::vector<std::filesystem::path>& paths, const RenderSettings& s) {
  std::vector<std::unique_ptr<Film>> films;
  try {
    for (const auto& p : paths) films.push_back(Film::open(p, /*writable=*/false, /*keepInRam=*/false));
  } catch (const FileError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  const FilmLayout layout = films.front()->layout();
  for (size_t i = 1; i < films.size(); ++i) {
    if (!(films[i]->layout() == layout)) {
      spdlog::error("'{}' has a different size or tile size than '{}'; only films of the same render can be merged",
                    pathToUtf8(paths[i]), pathToUtf8(paths[0]));
      return toInt(ExitCode::BadArguments);
    }
    if (films[i]->identity().scene_hash != films[0]->identity().scene_hash) {
      spdlog::error("'{}' was rendered from different scene files than '{}'", pathToUtf8(paths[i]), pathToUtf8(paths[0]));
      return toInt(ExitCode::BadArguments);
    }
  }
  uint32_t tiles = 0;
  for (const auto& f : films) tiles += f->tilesWithData();
  spdlog::info("merging {} films ({}x{}, {} tiles with data in total)", films.size(), layout.width, layout.height, tiles);

  // Per pixel: sample-weighted mean of every film that has samples there.
  const RowSource source = [&](const TileRect& rect, std::vector<Vec3f>& out) {
    std::vector<double> sum(rect.pixels() * 3, 0.0);
    std::vector<uint64_t> n(rect.pixels(), 0);
    std::vector<Vec3f> mean;
    std::vector<uint32_t> count;
    for (const auto& f : films) {
      f->readRect(rect, mean, &count);
      for (size_t i = 0; i < mean.size(); ++i) {
        for (int c = 0; c < 3; ++c) sum[i * 3 + c] += double(mean[i][c]) * count[i];
        n[i] += count[i];
      }
    }
    out.assign(rect.pixels(), Vec3f(0));
    for (size_t i = 0; i < out.size(); ++i)
      if (n[i]) out[i] = {float(sum[i * 3] / n[i]), float(sum[i * 3 + 1] / n[i]), float(sum[i * 3 + 2] / n[i])};
  };
  try {
    std::vector<std::string> notes;
    ImageMetadata md = {{"Software", versionString()}, {"MergedFrom", std::to_string(films.size()) + " films"}};
    writeImage(source, layout.width, layout.height, s, md, notes);
    for (const auto& note : notes) spdlog::info("{}", note);
  } catch (const ImageIoError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::OutOfDisk);
  } catch (const FileError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  spdlog::info("wrote {}", pathToUtf8(s.output));
  return toInt(ExitCode::Success);
}

}  // namespace pen
