#pragma once

#include <nlohmann/json_fwd.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/settings/render_settings.h"

namespace pen {

enum class OptionKind { Flag, Value };

// One persistent setting. The same table generates the CLI options, --help, the README CLI reference, and
// defines the keys accepted in .pen.json "render" and in the user config file.
struct SettingSpec {
  std::string_view key;    // JSON key, e.g. "max_bounces"
  std::string_view names;  // CLI names, e.g. "--max-bounces" or "-o,--output"
  std::string_view group;  // --help group: General, Render, Devices, Output, Film, Controller, Worker
  OptionKind kind;
  std::string_view values;        // shown in help, e.g. "0-1024"
  std::string_view default_text;  // shown in help
  bool default_parses;            // default_text is a valid value equal to the RenderSettings default
  std::string_view example;
  std::string_view help;
  void (*apply)(RenderSettings&, const nlohmann::json&);  // throws ValueError
};

std::span<const SettingSpec> settingSchema();
const SettingSpec* findSetting(std::string_view key);
std::string_view longFlag(const SettingSpec& spec);  // "--output" for "-o,--output"

// A settings error with the process exit code it should produce.
struct SettingsError : std::runtime_error {
  SettingsError(int exit_code, const std::string& msg) : std::runtime_error(msg), exit_code(exit_code) {}
  int exit_code;
};

// Applies one value; on failure throws SettingsError(1, "<where>: <message>").
void applySetting(RenderSettings& s, const SettingSpec& spec, const nlohmann::json& value, std::string_view where);

// Cross-field checks after every layer is applied: resolves the output format and bit depth, enforces
// format size limits, spp 0 needs a time limit, region and sample range fit. Throws SettingsError(1, ...).
void finalizeSettings(RenderSettings& s);

// Max pixels per side for a format, or 0 if the format has no practical limit at our sizes.
uint32_t formatMaxSide(ImageFormat f);

}  // namespace pen
