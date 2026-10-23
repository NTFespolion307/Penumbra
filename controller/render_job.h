#pragma once

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>

#include "controller/render_controller.h"
#include "core/film/film.h"
#include "core/film/session.h"
#include "core/package/package.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

// One render apart from its devices and workers: the checked settings, the scene and its package, the film and
// its session file (docs/GUIDE.md "Render lifecycle", steps 1 and 9-10). The terminal (solo_render) and the UI
// prepare and finish renders with the same functions.
struct RenderJob {
  RenderSettings settings;
  std::shared_ptr<Scene> scene;
  std::shared_ptr<const ScenePackage> package;
  std::unique_ptr<Film> film;
  Session session;
  std::filesystem::path session_path;
};

// Step 1 for a new render: checks the settings, packages and loads the scene, checks disk space, creates the film
// and writes the session file. Returns -1 when ready, else the exit code; the reason is logged.
int prepareNewRender(const RenderSettings& settings, RenderJob& job);

// The same for `--resume`: the session file's settings plus `cliOverrides` (settings that do not change pixels).
// job.session.hosting / join_code tell whether the render was hosting workers.
int prepareResumedRender(const std::filesystem::path& sessionFile, const nlohmann::json& cliOverrides, RenderJob& job);

// Writes the session file with the progress so far (atomically; a failure is logged, not thrown).
void saveSessionProgress(RenderJob& job, const RenderProgress& progress);

// Steps 9-10 once the controller is done: final checkpoint, session state, the output image (also for an
// interrupted render), the summary per device, and the film and session file removed when complete. Returns the
// exit code.
int finishRender(RenderJob& job, const RenderOutcome& outcome);

}  // namespace pen
