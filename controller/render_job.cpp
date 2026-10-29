#include "controller/render_job.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <fmt/format.h>

#include "core/exit_codes.h"
#include "core/output/output.h"
#include "core/output/writers.h"
#include "core/render/camera.h"
#include "core/scene/scene_loader.h"
#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/settings/serialize.h"
#include "core/system/file.h"
#include "core/system/hash.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"

namespace pen {

namespace {

// Checks that do not need the scene. Returns an exit code, or -1 to continue.
int validate(const RenderSettings& s) {
  if (s.integrator == Integrator::DebugBvh && s.backend != BackendKind::Cuda) {
    spdlog::error("--integrator debug:bvh needs traversal step counts, which only the CUDA backend's software BVH "
                  "reports (Embree and Vulkan ray tracing do not); add --backend cuda");
    return toInt(ExitCode::BadArguments);
  }
  if (s.denoise != Denoiser::None) spdlog::warn("--denoise {} is built in milestone M11; not applied", toString(s.denoise));
  if (!s.aov.empty()) spdlog::warn("--aov is built in milestone M11; no extra passes are written");
  return -1;
}

// Packaging is the validation step of every render (docs/GUIDE.md "Package format"): every file the scene needs
// is found and hashed before anything renders; workers download exactly these files.
int buildPackageChecked(const RenderSettings& s, std::shared_ptr<const ScenePackage>& pkg) {
  try {
    const auto t0 = std::chrono::steady_clock::now();
    auto p = std::make_shared<ScenePackage>(buildPackage(s));
    spdlog::info("scene package {}: {} files, {}, {} chunks; built in {:.2f}s", toHex(p->hash()), p->files.size(),
                 formatBytes(p->total_bytes), p->chunkCount(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    pkg = std::move(p);
  } catch (const SceneError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::SceneLoadError);
  }
  return -1;
}

int loadSceneChecked(const RenderSettings& s, std::shared_ptr<Scene>& scene) {
  scene = std::make_shared<Scene>();
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
  return -1;
}

// docs/GUIDE.md resolution table: up to 15k the film is in RAM; 20k and 40k when the RAM budget allows; above
// that (and whenever it does not fit) it lives only on disk.
bool filmInRam(const RenderSettings& s, const FilmLayout& layout) {
  const uint64_t bytes = uint64_t(layout.width) * layout.height * 16;
  const uint64_t budget = s.ram_limit ? s.ram_limit : querySystem().ram_total / 4 * 3;
  const bool tooBig = uint64_t(layout.width) * layout.height > uint64_t(40960) * 23040;
  const bool inRam = !tooBig && bytes <= budget / 2;  // leave half the budget for the scene and the OS
  spdlog::info("film {}x{} ({}): {} (RAM budget {}{})", layout.width, layout.height, formatBytes(bytes),
               inRam ? "in RAM, mirrored to disk" : "on disk only", formatBytes(budget),
               s.ram_limit ? ", --ram-limit" : ", 75% of physical RAM");
  return inRam;
}

// Free space for the film still to be written plus the output. Returns an exit code, or -1 to continue.
int checkDisk(const RenderSettings& s, const std::filesystem::path& filmPath, uint64_t filmBytesNeeded) {
  const TileRect out = outputRect(s);
  const uint64_t outBytes = estimateOutputBytes(*s.format, s.bit_depth, out.width(), out.height());
  const std::filesystem::path filmDir = std::filesystem::absolute(filmPath).parent_path();
  const std::filesystem::path outDir = std::filesystem::absolute(s.output).parent_path();
  const uint64_t filmFree = freeDiskBytes(filmDir), outFree = freeDiskBytes(outDir);
  const bool sameVolume = filmFree == outFree && filmDir.root_name() == outDir.root_name();
  const auto refuse = [&](const std::filesystem::path& dir, uint64_t need, uint64_t have) {
    spdlog::error("not enough disk space in '{}': this render needs {} (film {} + output up to {}), {} free. Use "
                  "--film-dir on a bigger drive, a smaller --res, or free some space.",
                  pathToUtf8(dir), formatBytes(need), formatBytes(filmBytesNeeded), formatBytes(outBytes),
                  formatBytes(have));
    return toInt(ExitCode::OutOfDisk);
  };
  if (sameVolume) {
    if (filmFree < filmBytesNeeded + outBytes) return refuse(filmDir, filmBytesNeeded + outBytes, filmFree);
  } else {
    if (filmFree < filmBytesNeeded) return refuse(filmDir, filmBytesNeeded, filmFree);
    if (outFree < outBytes) return refuse(outDir, outBytes, outFree);
  }
  spdlog::info("disk: film {} in '{}', output up to {}; {} free", formatBytes(filmBytesNeeded), pathToUtf8(filmDir),
               formatBytes(outBytes), formatBytes(filmFree));
  return -1;
}

ImageMetadata metadataFor(const RenderSettings& s, const RenderOutcome& outcome) {
  std::string deviceNames;
  for (const auto& d : outcome.devices) deviceNames += (deviceNames.empty() ? "" : "; ") + d.name;
  const TileRect r = outputRect(s);
  ImageMetadata md = {
      {"Software", versionString()},
      {"Scene", pathToUtf8(s.scene)},
      {"Resolution", fmt::format("{}x{}", s.outputWidth(), s.outputHeight())},
      {"Samples", s.spp ? std::to_string(s.spp) : std::string("until the time limit")},
      {"MaxBounces", std::to_string(s.max_bounces)},
      {"Integrator", std::string(toString(s.integrator))},
      {"Seed", std::to_string(s.seed)},
      {"Backend", std::string(toString(s.backend))},
      {"Devices", deviceNames},
      {"RenderTime", fmt::format("{:.2f}s", outcome.seconds)},
      {"Date", localTimestamp()},
  };
  if (s.region) md["Region"] = fmt::format("{},{},{},{}", r.x0, r.y0, r.x1, r.y1);
  if (s.sample_range) md["SampleRange"] = fmt::format("{}-{}", s.sample_range->first, s.sample_range->last);
  if (!outcome.complete) md["Incomplete"] = "true";
  return md;
}

void saveSessionFile(Session& session, const std::filesystem::path& path, const RenderProgress& p, const Film& film) {
  session.updated = localTimestamp();
  session.tiles_done = p.items_done;
  session.tiles_total = film.layout().tileCount();
  session.samples_done = p.samples_done;
  session.samples_total = p.samples_total;
  try {
    session.save(path);
  } catch (const FileError& e) {
    spdlog::error("cannot save the session file: {}", e.what());
  }
}

}  // namespace

int prepareNewRender(const RenderSettings& s, RenderJob& job) {
  if (const int rc = validate(s); rc >= 0) return rc;
  std::shared_ptr<const ScenePackage> pkg;
  if (const int rc = buildPackageChecked(s, pkg); rc >= 0) return rc;
  std::shared_ptr<Scene> scene;
  if (const int rc = loadSceneChecked(s, scene); rc >= 0) return rc;

  const uint32_t w = s.outputWidth(), h = s.outputHeight();
  const FilmLayout layout = FilmLayout::make(w, h, filmTileSize(w, h, s.tile_size));
  const std::filesystem::path filmPath = filmPathFor(s);
  const std::filesystem::path sessionPath = sessionPathFor(filmPath);
  const bool inRam = filmInRam(s, layout);
  if (s.spp == 0 && !inRam) {
    spdlog::error("--spp 0 refines the whole image pass by pass, which needs the film in RAM; this film does not "
                  "fit (use a smaller --res, raise --ram-limit, or give --spp)");
    return toInt(ExitCode::BadArguments);
  }
  // RAM films rewrite every tile at each checkpoint, alternating two slots per tile: up to twice the film.
  if (const int rc = checkDisk(s, filmPath, Film::fileBytes(layout) * (inRam ? 2 : 1)); rc >= 0) return rc;

  std::error_code ec;
  if (std::filesystem::exists(sessionPath, ec))
    spdlog::warn("replacing an earlier session at '{}' (to continue that render instead, use --resume)",
                 pathToUtf8(sessionPath));

  Session session;
  session.app_version = std::string(appVersion());
  session.git_hash = std::string(gitHash());
  session.settings = s;
  session.film = filmPath;
  session.scene_hash = sceneFingerprint(s);
  session.settings_hash = pixelSettingsHash(s);
  session.created = session.updated = localTimestamp();

  std::unique_ptr<Film> film;
  try {
    if (!filmPath.parent_path().empty()) std::filesystem::create_directories(filmPath.parent_path());
    film = Film::create(filmPath, layout, {session.settings_hash, session.scene_hash}, inRam);
    session.save(sessionPath);
  } catch (const DiskFullError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::OutOfDisk);
  } catch (const FileError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  } catch (const std::filesystem::filesystem_error& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  spdlog::info("film {}, session {}", pathToUtf8(filmPath), pathToUtf8(sessionPath));
  job.settings = s;
  job.scene = std::move(scene);
  job.package = std::move(pkg);
  job.film = std::move(film);
  job.session = std::move(session);
  job.session_path = sessionPath;
  return -1;
}

int prepareResumedRender(const std::filesystem::path& sessionFile, const nlohmann::json& cliOverrides, RenderJob& job) {
  Session session;
  try {
    session = Session::load(sessionFile);
  } catch (const SettingsError& e) {
    spdlog::error("{}", e.what());
    return e.exit_code;
  }
  if (session.git_hash != gitHash()) {
    spdlog::error("this film was rendered by Penumbra {} (git {}), and this is {} (git {}). Tiles from different "
                  "builds may not match, so resume with that version or start a new render.",
                  session.app_version, session.git_hash, appVersion(), gitHash());
    return toInt(ExitCode::BadArguments);
  }

  if (session.hosting)
    spdlog::info("this render was hosting workers: hosting again on the same port and code; workers started with "
                 "--stay reconnect on their own");
  RenderSettings s = session.settings;
  try {
    for (const auto& [key, value] : cliOverrides.items()) {
      if (key == "scene" || isPixelSetting(key)) {
        spdlog::error("--{} changes the image, so it cannot change when resuming; start a new render instead",
                      key == "scene" ? std::string("scene") : std::string(longFlag(*findSetting(key))).substr(2));
        return toInt(ExitCode::BadArguments);
      }
      if (const SettingSpec* spec = findSetting(key)) applySetting(s, *spec, value, longFlag(*spec));
    }
    if (cliOverrides.contains("output") && !cliOverrides.contains("format")) s.format.reset();
    if (cliOverrides.contains("output") && !cliOverrides.contains("bit_depth")) s.bit_depth = 0;
    finalizeSettings(s);
  } catch (const SettingsError& e) {
    spdlog::error("{}", e.what());
    return e.exit_code;
  }
  session.settings = s;

  spdlog::info("resuming {} ({} of {} tiles done, state {})", pathToUtf8(sessionFile), session.tiles_done,
               session.tiles_total, session.state);
  if (sceneFingerprint(s) != session.scene_hash) {
    spdlog::error("the scene files changed since this render started (scene, .pen.json or HDRI); start a new "
                  "render instead");
    return toInt(ExitCode::SceneLoadError);
  }
  if (const int rc = validate(s); rc >= 0) return rc;
  std::shared_ptr<const ScenePackage> pkg;
  if (const int rc = buildPackageChecked(s, pkg); rc >= 0) return rc;
  std::shared_ptr<Scene> scene;
  if (const int rc = loadSceneChecked(s, scene); rc >= 0) return rc;

  const uint32_t w = s.outputWidth(), h = s.outputHeight();
  const FilmLayout layout = FilmLayout::make(w, h, filmTileSize(w, h, s.tile_size));
  std::unique_ptr<Film> film;
  try {
    film = Film::open(session.film, /*writable=*/true, filmInRam(s, layout));
  } catch (const FileError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::BadArguments);
  }
  if (!(film->layout() == layout) || film->identity().settings_hash != session.settings_hash ||
      film->identity().scene_hash != session.scene_hash) {
    spdlog::error("the film '{}' does not belong to this session (different size, settings or scene)",
                  pathToUtf8(session.film));
    return toInt(ExitCode::BadArguments);
  }
  const uint64_t onDisk = std::filesystem::file_size(session.film);
  const uint64_t remaining = Film::fileBytes(layout) > onDisk ? Film::fileBytes(layout) - onDisk : 0;
  if (const int rc = checkDisk(s, session.film, remaining); rc >= 0) return rc;
  job.settings = s;
  job.scene = std::move(scene);
  job.package = std::move(pkg);
  job.film = std::move(film);
  job.session = std::move(session);
  job.session_path = sessionFile;
  return -1;
}

void saveSessionProgress(RenderJob& job, const RenderProgress& p) {
  saveSessionFile(job.session, job.session_path, p, *job.film);
}

int finishRender(RenderJob& job, const RenderOutcome& outcomeIn) {
  const RenderSettings& s = job.settings;
  RenderOutcome outcome = outcomeIn;
  // Final checkpoint and session state.
  try {
    job.film->checkpoint();
  } catch (const FileError& e) {
    spdlog::error("final checkpoint failed: {}", e.what());
    outcome.out_of_disk = outcome.out_of_disk || dynamic_cast<const DiskFullError*>(&e) != nullptr;
    if (outcome.error.empty()) outcome.error = e.what();
  }
  job.session.state = outcome.complete       ? "complete"
                      : outcome.out_of_disk  ? "out_of_disk"
                      : outcome.interrupted  ? "interrupted"
                      : outcome.time_limited ? "time_limit"
                                             : "rendering";
  saveSessionProgress(job, outcome.progress);

  if (!outcome.error.empty() && !outcome.out_of_disk) {
    spdlog::error("the film could not be written; resume later with: penumbra --terminal --resume \"{}\"",
                  pathToUtf8(job.session_path));
    return toInt(ExitCode::OutOfDisk);
  }
  if (outcome.out_of_disk) {
    spdlog::error("out of disk: the film is checkpointed. Free some space, then continue with: penumbra --terminal "
                  "--resume \"{}\"",
                  pathToUtf8(job.session_path));
    return toInt(ExitCode::OutOfDisk);
  }

  // Output (also for interrupted renders: "the current image is saved").
  try {
    std::vector<std::string> notes;
    writeOutput(*job.film, s, metadataFor(s, outcome), notes);
    for (const auto& n : notes) spdlog::info("{}", n);
  } catch (const ImageIoError& e) {
    spdlog::error("{}", e.what());
    return toInt(ExitCode::OutOfDisk);
  }

  uint64_t samples = 0;
  for (const auto& d : outcome.devices) samples += d.samples;
  spdlog::info("{} in {:.2f}s: {:.2f} M samples, {:.2f} M samples/s; wrote {}",
               outcome.complete ? "done" : "stopped", outcome.seconds, samples / 1e6,
               outcome.seconds > 0 ? samples / 1e6 / outcome.seconds : 0.0, pathToUtf8(s.output));
  for (const auto& d : outcome.devices)
    spdlog::info("  {}: {} items, {:.2f} M samples, {:.2f}s{}", d.name, d.items, d.samples / 1e6, d.seconds,
                 d.failed ? " (failed)" : "");
  if (outcome.reissued) spdlog::info("{} lease(s) were handed out again (late or from a failed device)", outcome.reissued);
  if (outcome.results && outcome.seconds > 0)
    spdlog::info("controller: {} results ({:.0f}/s); unpacking {:.1f}s ({:.0f}% of one core), merging {:.1f}s "
                 "({:.0f}% of one core)",
                 outcome.results, outcome.results / outcome.seconds, outcome.decode_seconds,
                 100 * outcome.decode_seconds / outcome.seconds, outcome.merge_seconds,
                 100 * outcome.merge_seconds / outcome.seconds);
  if (outcome.seconds > 0)
    spdlog::info("controller: handing out work {:.1f}s ({:.0f}%; tail duplicates {:.1f}s), late check {:.1f}s "
                 "({:.0f}%), network thread busy {:.1f}s ({:.0f}%; decrypting {:.1f}s)",
                 outcome.serve_seconds, 100 * outcome.serve_seconds / outcome.seconds, outcome.tail_seconds,
                 outcome.late_seconds,
                 100 * outcome.late_seconds / outcome.seconds, outcome.network_seconds,
                 100 * outcome.network_seconds / outcome.seconds, outcome.tls_seconds);
  if (outcome.duplicated)
    spdlog::info("tail duplication: {} item(s) copied to idle devices, {} copy(ies) finished first", outcome.duplicated,
                 outcome.duplicate_wins);
  if (outcome.nan_count) spdlog::warn("{} non-finite samples were replaced by 0", outcome.nan_count);

  const std::filesystem::path filmPath = job.film->path();
  job.film->close();
  if (outcome.complete && !s.region && !s.sample_range) {
    std::error_code ec;
    std::filesystem::remove(filmPath, ec);
    std::filesystem::remove(job.session_path, ec);
  } else if (outcome.complete) {
    spdlog::info("film kept for `penumbra merge`: {}", pathToUtf8(filmPath));
  } else {
    spdlog::warn("the render is not finished; unrendered tiles are black in the output. Continue with: penumbra "
                 "--terminal --resume \"{}\"",
                 pathToUtf8(job.session_path));
  }
  if (outcome.interrupted) return toInt(ExitCode::Interrupted);
  return toInt(ExitCode::Success);
}

}  // namespace pen
