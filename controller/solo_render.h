#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

#include "controller/render_controller.h"
#include "core/settings/render_settings.h"

namespace pen {

struct LocalRenderOptions {
  std::function<void(const RenderProgress&)> progress;
  const std::atomic<bool>* interrupt = nullptr;  // Ctrl+C
  bool host = false;  // --host: accept workers on --port while rendering (docs/GUIDE.md "Controller and workers")
  std::string code;   // --code / PENUMBRA_CODE: a fixed join code; empty = the saved one (created on first use)
};

// A headless controller render (`--terminal`, with `--host` also on workers that join): load the scene, create
// the film (RAM or disk) and session file, render through controller -> scheduler -> local and remote workers
// -> film, checkpoint, write the output. Returns the process exit code (0, or 1-5 per docs/GUIDE.md).
int runLocalRender(const RenderSettings& settings, const LocalRenderOptions& options);

// `--resume <session.json>`: continues an interrupted render. `cliOverrides` holds settings given on the
// command line; only settings that do not change pixels (output, logging, devices...) may differ.
int resumeLocalRender(const std::filesystem::path& sessionFile, const nlohmann::json& cliOverrides,
                      const LocalRenderOptions& options);

// `penumbra merge a.penfilm b.penfilm -o out.exr`: combines partial films (regions or sample ranges) into one
// image. `settings` supplies the output options (path, format, exposure...).
int mergeFilmsToImage(const std::vector<std::filesystem::path>& films, const RenderSettings& settings);

}  // namespace pen
