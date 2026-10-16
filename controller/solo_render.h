#pragma once

#include <functional>

#include "controller/render_controller.h"
#include "core/settings/render_settings.h"

namespace pen {

// A headless controller render on this machine's devices (`--terminal`): load scene, pick devices, render
// through the controller -> scheduler -> local worker -> film path, write the output. Returns the exit code.
int runLocalRender(const RenderSettings& settings, const std::function<void(const RenderProgress&)>& progress);

}  // namespace pen
