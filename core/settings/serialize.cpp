#include "core/settings/serialize.h"

#include <array>
#include <fmt/format.h>

#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/system/hash.h"
#include "core/system/paths.h"

namespace pen {

using nlohmann::json;

namespace {

constexpr std::array kPixelKeys = {"res",  "scale",      "spp",         "max_bounces", "max_diffuse",
                                   "max_glossy", "max_transmission", "integrator", "no_rr", "ao_distance",
                                   "seed", "camera",     "region",      "sample_range", "tile_size"};

json optionalBounces(const std::optional<uint32_t>& v) { return v ? json(*v) : json("auto"); }
std::string bytes(uint64_t v) { return std::to_string(v); }

}  // namespace

bool isPixelSetting(std::string_view key) {
  for (const char* k : kPixelKeys)
    if (key == k) return true;
  return false;
}

json settingsToJson(const RenderSettings& s) {
  json j;
  j["log_level"] = toString(s.log_level);
  j["log_file"] = pathToUtf8(s.log_file);
  j["progress"] = toString(s.progress);
  j["scene"] = pathToUtf8(s.scene);
  j["camera"] = s.camera;
  j["res"] = s.res.preset.empty() ? fmt::format("{}x{}", s.res.width, s.res.height) : s.res.preset;
  j["scale"] = s.scale;
  j["spp"] = s.spp;
  j["max_bounces"] = s.max_bounces;
  j["max_diffuse"] = optionalBounces(s.max_diffuse);
  j["max_glossy"] = optionalBounces(s.max_glossy);
  j["max_transmission"] = optionalBounces(s.max_transmission);
  j["integrator"] = toString(s.integrator);
  j["no_rr"] = s.no_rr;
  j["ao_distance"] = s.ao_distance;
  j["seed"] = s.seed;
  j["time_limit"] = s.time_limit_s ? formatDuration(s.time_limit_s) : "none";
  j["backend"] = toString(s.backend);
  j["devices"] = s.devices;
  j["threads"] = s.threads;
  j["tile_size"] = s.tile_size;
  j["vram_limit"] = s.vram_limit ? bytes(s.vram_limit) : "auto";
  j["ram_limit"] = s.ram_limit ? bytes(s.ram_limit) : "auto";
  j["no_texture_downsize"] = s.no_texture_downsize;
  j["output"] = pathToUtf8(s.output);
  j["format"] = s.format ? json(toString(*s.format)) : json("auto");
  j["bit_depth"] = s.bit_depth ? json(s.bit_depth) : json("auto");
  j["denoise"] = toString(s.denoise);
  j["tonemap"] = toString(s.tonemap);
  j["exposure"] = s.exposure;
  j["aov"] = s.aov;
  j["film_dir"] = pathToUtf8(s.film_dir);
  j["checkpoint"] = formatDuration(s.checkpoint_s);
  j["region"] = s.region ? fmt::format("{},{},{},{}", s.region->x0, s.region->y0, s.region->x1, s.region->y1) : "none";
  j["sample_range"] = s.sample_range ? fmt::format("{}-{}", s.sample_range->first, s.sample_range->last) : "none";
  j["port"] = s.port;
  j["bind"] = s.bind;
  j["approve_workers"] = s.approve_workers;
  j["max_workers"] = s.max_workers;
  j["no_upnp"] = s.no_upnp;
  j["no_local"] = s.no_local;
  j["upload_limit"] = s.upload_limit ? bytes(s.upload_limit) : "unlimited";
  j["worker_name"] = s.worker_name;
  j["stay"] = s.stay;
  j["cache_dir"] = pathToUtf8(s.cache_dir);
  j["cache_size"] = bytes(s.cache_size);

  j["scene_description"] = pathToUtf8(s.scene_description);
  j["environment"] = {{"hdri", pathToUtf8(s.environment.hdri)},
                      {"color", s.environment.color},
                      {"strength", s.environment.strength},
                      {"rotation_deg", s.environment.rotation_deg}};
  const CameraOverrides& co = s.camera_overrides;
  j["camera_overrides"] = {{"projection", co.projection},
                           {"fstop", co.fstop},
                           {"focus_distance", co.focus_distance},
                           {"sensor_width_mm", co.sensor_width_mm}};
  return j;
}

RenderSettings settingsFromJson(const json& j) {
  RenderSettings s;
  try {
    for (const auto& [key, value] : j.items()) {
      if (key == "scene") {
        s.scene = pathFromUtf8(value.get<std::string>());
      } else if (key == "scene_description") {
        s.scene_description = pathFromUtf8(value.get<std::string>());
      } else if (key == "environment") {
        s.environment.hdri = pathFromUtf8(value.at("hdri").get<std::string>());
        s.environment.color = value.at("color").get<std::array<double, 3>>();
        s.environment.strength = value.at("strength").get<double>();
        s.environment.rotation_deg = value.at("rotation_deg").get<double>();
      } else if (key == "camera_overrides") {
        s.camera_overrides.projection = value.at("projection").get<std::string>();
        s.camera_overrides.fstop = value.at("fstop").get<double>();
        s.camera_overrides.focus_distance = value.at("focus_distance").get<double>();
        s.camera_overrides.sensor_width_mm = value.at("sensor_width_mm").get<double>();
      } else if (const SettingSpec* spec = findSetting(key)) {
        if (key == "log_file" || key == "film_dir" || key == "cache_dir") {
          // Empty paths mean "default" and are not valid setter input.
          if (value.is_string() && value.get<std::string>().empty()) continue;
        }
        applySetting(s, *spec, value, key);
      }
    }
  } catch (const json::exception& e) {
    throw SettingsError(1, fmt::format("invalid settings: {}", e.what()));
  }
  finalizeSettings(s);
  return s;
}

uint64_t pixelSettingsHash(const RenderSettings& s) {
  const json all = settingsToJson(s);
  json pixel = json::object();
  for (const char* k : kPixelKeys) pixel[k] = all[k];
  pixel["environment"] = all["environment"];
  pixel["camera_overrides"] = all["camera_overrides"];
  return hash64(pixel.dump());
}

}  // namespace pen
