#pragma once

#include <functional>

#include "controller/render_controller.h"
#include "core/settings/render_settings.h"

namespace pen::app {

// --progress bar (stderr, redrawn in place; falls back to plain when stderr is not a terminal),
// plain (a line on stderr every 5 s) or json (one object per second on stdout).
std::function<void(const RenderProgress&)> makeProgressPrinter(ProgressStyle style);

}  // namespace pen::app
