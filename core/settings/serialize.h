#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string_view>

#include "core/settings/render_settings.h"

namespace pen {

// Every setting as JSON (schema keys plus scene, scene_description, environment, camera_overrides), in the
// same value syntax the CLI and .pen.json accept, so settingsFromJson(settingsToJson(s)) == s.
nlohmann::json settingsToJson(const RenderSettings& s);
// Throws SettingsError on invalid values; the result is finalized.
RenderSettings settingsFromJson(const nlohmann::json& j);

// Settings that change pixels. A resumed render must keep them; the rest (output, logging, devices) may change.
bool isPixelSetting(std::string_view key);
// Fingerprint of everything that affects pixels: pixel settings, environment and camera overrides.
uint64_t pixelSettingsHash(const RenderSettings& s);

}  // namespace pen
