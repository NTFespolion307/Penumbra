#pragma once

#include "core/settings/render_settings.h"

namespace pen::ui {

// Runs the UI as the controller (docs/GUIDE.md "UI"): `settings` are the resolved defaults and command line, with
// the scene if one was given. Returns the process exit code.
int run(const RenderSettings& settings);

}  // namespace pen::ui
