#include "controller/solo_render.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fmt/format.h>
#include <set>

#include "controller/hosting/hosting_session.h"
#include "controller/render_job.h"
#include "core/exit_codes.h"
#include "core/film/film.h"
#include "core/film/session.h"
#include "core/settings/serialize.h"
#include "core/output/output.h"
#include "core/output/writers.h"
#include "core/system/paths.h"
#include "core/version.h"
#include "net/join_code.h"
#include "worker/device_manager.h"

namespace pen {

namespace {

// The join code: --code / PENUMBRA_CODE, else the one saved in the user data folder.
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

// This machine's devices for `s` (none with --no-local). Returns -1 when ready, else the exit code (logged).
int createLocalDevices(const RenderSettings& s, DeviceSelection& devices) {
  if (s.no_local) {
    spdlog::info("--no-local: this machine coordinates only; workers do all the rendering");
    return -1;
  }
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
  return -1;
}

// --host: the join code, then listening on --port. Returns -1 when hosting, else the exit code (logged).
int startHostingFor(HostingSession& session, const RenderSettings& s, const LocalRenderOptions& options,
                    std::string& code) {
  if (!joinCode(options.code, code)) return toInt(ExitCode::BadArguments);
  try {
    session.startHosting(s, code);
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  if (s.no_local) spdlog::info("waiting for workers to join");
  return -1;
}

// The session file records that the render hosts workers with `code`, so --resume hosts again with it.
void markHosting(RenderJob& job, const std::string& code) {
  job.session.hosting = true;
  job.session.join_code = code;
  RenderProgress sofar;  // what the session file already says (a resumed render keeps its counts)
  sofar.items_done = job.session.tiles_done;
  sofar.items_total = job.film->layout().tileCount();
  sofar.samples_done = job.session.samples_done;
  sofar.samples_total = job.session.samples_total;
  saveSessionProgress(job, sofar);
}

// Renders a prepared job through `session` (the same path the UI uses): the local devices, and when hosting every
// worker that joins, then writes the output. `more`: another render of this session follows (--batch).
int renderPrepared(HostingSession& session, RenderJob& job, DeviceSelection devices, const LocalRenderOptions& options,
                   bool more) {
  RenderRunOptions run;
  run.interrupt = options.interrupt;
  run.progress = options.progress;
  run.after_checkpoint = [&](const RenderProgress& p) { saveSessionProgress(job, p); };
  run.more_renders = more;
  RenderOutcome outcome;
  try {
    outcome = session.render(job, std::move(devices.devices), run);
  } catch (const std::exception& e) {
    spdlog::error("render failed: {}", e.what());
    return toInt(ExitCode::NoUsableDevice);
  }
  return finishRender(job, outcome);
}

int renderAndFinish(RenderJob& job, const LocalRenderOptions& options) {
  DeviceSelection devices;
  if (const int rc = createLocalDevices(job.settings, devices); rc >= 0) return rc;
  HostingSession session;
  if (options.host) {
    std::string code;
    if (const int rc = startHostingFor(session, job.settings, options, code); rc >= 0) return rc;
    markHosting(job, code);
  }
  const int rc = renderPrepared(session, job, std::move(devices), options, /*more=*/false);
  // Workers got RenderComplete; give them a moment to say Bye before closing their connections.
  session.stopHosting(std::chrono::seconds(rc == toInt(ExitCode::NoUsableDevice) ? 0 : 5));
  return rc;
}

// A batch job: continues from the checkpoint an interrupted run of the batch left (same pixel settings), else starts
// fresh. Returns -1 when ready, else the exit code.
int prepareBatchJob(const BatchJob& bj, RenderJob& job) {
  const std::filesystem::path sessionFile = sessionPathFor(filmPathFor(bj.settings));
  std::error_code ec;
  if (std::filesystem::is_regular_file(sessionFile, ec)) {
    try {
      const Session old = Session::load(sessionFile);
      if (old.settings_hash == pixelSettingsHash(bj.settings) && old.state != "complete") {
        spdlog::info("{}: continuing from the checkpoint in {}", bj.name, pathToUtf8(sessionFile));
        return prepareResumedRender(sessionFile, nlohmann::json::object(), job);
      }
      spdlog::info("{}: {} is from a render with other settings; starting over", bj.name, pathToUtf8(sessionFile));
    } catch (const std::exception& e) {
      spdlog::info("{}: cannot use {} ({}); starting over", bj.name, pathToUtf8(sessionFile), e.what());
    }
  }
  return prepareNewRender(bj.settings, job);
}

}  // namespace

int runLocalRender(const RenderSettings& s, const LocalRenderOptions& options) {
  RenderJob job;
  if (const int rc = prepareNewRender(s, job); rc >= 0) return rc;
  return renderAndFinish(job, options);
}

int runBatch(const std::filesystem::path& batchFile, const std::vector<BatchJob>& jobs,
             const LocalRenderOptions& options) {
  const std::filesystem::path stateFile = batchStatePath(batchFile);
  std::set<uint64_t> done = loadBatchState(stateFile);
  std::vector<bool> skip(jobs.size(), false);
  size_t toRender = 0;
  for (size_t i = 0; i < jobs.size(); ++i) {
    std::error_code ec;
    skip[i] = done.count(jobs[i].fingerprint) && std::filesystem::exists(jobs[i].settings.output, ec);
    if (!skip[i]) ++toRender;
  }
  spdlog::info("batch {}: {} job(s){}", pathToUtf8(batchFile), jobs.size(),
               toRender < jobs.size()
                   ? fmt::format(", {} finished in an earlier run and skipped (delete {} to render them again)",
                                 jobs.size() - toRender, pathToUtf8(stateFile))
                   : std::string());

  HostingSession session;
  std::string code;
  if (options.host && toRender > 0) {
    const size_t first = size_t(std::find(skip.begin(), skip.end(), false) - skip.begin());
    if (const int rc = startHostingFor(session, jobs[first].settings, options, code); rc >= 0) return rc;
  }

  struct Row {
    std::string name, output, status;
    double seconds = 0;
  };
  std::vector<Row> rows;
  int firstFailure = 0;
  bool stopped = false;
  size_t started = 0;
  for (size_t i = 0; i < jobs.size(); ++i) {
    const BatchJob& bj = jobs[i];
    Row row{bj.name, pathToUtf8(bj.settings.output), "", 0};
    if (skip[i]) {
      row.status = "skipped (finished in an earlier run)";
      rows.push_back(row);
      continue;
    }
    if (stopped || (options.interrupt && options.interrupt->load())) {
      stopped = true;
      row.status = "not started";
      rows.push_back(row);
      continue;
    }
    ++started;
    const bool more = std::find(skip.begin() + std::ptrdiff_t(i) + 1, skip.end(), false) != skip.end();
    spdlog::info("batch job {} of {}: {} -> {}", started, toRender, bj.name, row.output);
    const auto t0 = std::chrono::steady_clock::now();
    RenderJob job;
    int rc = prepareBatchJob(bj, job);
    if (rc < 0) {
      DeviceSelection devices;
      rc = createLocalDevices(job.settings, devices);
      if (rc < 0) {
        if (options.host) markHosting(job, code);
        rc = renderPrepared(session, job, std::move(devices), options, more);
      }
    }
    row.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (rc == 0) {
      row.status = "done";
      done.insert(bj.fingerprint);
      try {
        saveBatchState(stateFile, done);
      } catch (const std::exception& e) {
        spdlog::warn("cannot write {}: {} (running the batch again renders this job again)", pathToUtf8(stateFile),
                     e.what());
      }
    } else {
      row.status = rc == toInt(ExitCode::Interrupted) ? "interrupted (checkpoint kept)"
                                                       : fmt::format("failed (exit code {})", rc);
      if (firstFailure == 0) firstFailure = rc;
      if (rc == toInt(ExitCode::Interrupted)) stopped = true;
    }
    rows.push_back(row);
  }
  // Workers got RenderComplete; give them a moment to say Bye before closing their connections.
  if (session.hosting()) session.stopHosting(std::chrono::seconds(5));

  spdlog::info("batch summary:");
  for (const Row& r : rows)
    spdlog::info("  {}: {}{} -> {}", r.name, r.status, r.seconds > 0 ? fmt::format(" in {:.1f} s", r.seconds) : "",
                 r.output);
  if (firstFailure == 0) {
    std::error_code ec;
    std::filesystem::remove(stateFile, ec);
    spdlog::info("batch finished: every job is done");
  } else if (stopped) {
    spdlog::info("batch stopped; run the same command again to continue (finished jobs are skipped, the interrupted "
                 "one continues from its checkpoint)");
  } else {
    spdlog::info("batch finished with failures; run the same command again to retry them (finished jobs are skipped)");
  }
  return firstFailure;
}

int resumeLocalRender(const std::filesystem::path& sessionFile, const nlohmann::json& cliOverrides,
                      const LocalRenderOptions& options) {
  RenderJob job;
  if (const int rc = prepareResumedRender(sessionFile, cliOverrides, job); rc >= 0) return rc;
  LocalRenderOptions opts = options;
  if (job.session.hosting) {
    // The render was hosting: host again with the same code, so --stay workers find it.
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
