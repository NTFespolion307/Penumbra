#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "core/settings/render_settings.h"

namespace pen {

// The layers that make up the effective settings. Precedence, highest first:
// command line -> .pen.json "render" section -> user config file -> built-in defaults.
struct SettingsSources {
  nlohmann::json cli = nlohmann::json::object();     // setting key -> value given on the command line
  std::optional<std::filesystem::path> config_file;  // --config (must exist); unset: the per-OS user config
  bool load_user_config = true;                      // tests turn this off for reproducibility
};

struct ResolvedSettings {
  RenderSettings settings;            // finalized (format and bit depth resolved, cross-checked)
  std::filesystem::path config_file;  // config file that was read, empty if none
  std::vector<std::string> warnings;  // unknown keys, ignored keys; log these once logging is up
};

// Throws SettingsError: exit code 1 for bad values or config files, 2 for scene / .pen.json problems.
ResolvedSettings resolveSettings(const SettingsSources& sources);

}  // namespace pen
