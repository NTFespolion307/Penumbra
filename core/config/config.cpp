#include "core/config/config.h"

#include <algorithm>
#include <fmt/format.h>
#include <fstream>

#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/system/paths.h"

namespace pen {

using nlohmann::json;

namespace {

enum class Layer { ConfigFile, SceneDescription, CommandLine };

json readJsonObject(const std::filesystem::path& file, int exitCode, std::string_view what) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw SettingsError(exitCode, fmt::format("cannot open {} '{}'", what, pathToUtf8(file)));
  json j;
  try {
    j = json::parse(in, nullptr, true, /*ignore_comments=*/true);
  } catch (const json::parse_error& e) {
    throw SettingsError(exitCode, fmt::format("{} '{}' is not valid JSON: {}", what, pathToUtf8(file), e.what()));
  }
  if (!j.is_object()) throw SettingsError(exitCode, fmt::format("{} '{}' must be a JSON object", what, pathToUtf8(file)));
  return j;
}

void applyLayer(RenderSettings& s, const json& layer, Layer kind, std::string_view source,
                std::vector<std::string>& warnings) {
  for (const auto& [key, value] : layer.items()) {
    if (key == "code" || key == "join_code") {
      warnings.push_back(fmt::format("{}: the join code is never read from a file; use --code or PENUMBRA_CODE "
                                     "(ignored)", source));
      continue;
    }
    if (key == "scene" && kind != Layer::CommandLine) {
      warnings.push_back(fmt::format("{}: 'scene' is not a setting here (ignored)", source));
      continue;
    }
    const SettingSpec* spec = findSetting(key);
    if (!spec) {
      warnings.push_back(fmt::format("{}: unknown setting '{}' (ignored)", source, key));
      continue;
    }
    const std::string where =
        kind == Layer::CommandLine ? std::string(longFlag(*spec)) : fmt::format("{}: {}", source, key);
    applySetting(s, *spec, value, where);
  }
}

void loadSceneDescription(RenderSettings& s, const std::filesystem::path& file, std::vector<std::string>& warnings) {
  const json pen = readJsonObject(file, 2, "scene description");
  const std::string src = pathToUtf8(file);
  const std::filesystem::path base = file.parent_path();
  const auto fail = [&](const std::string& msg) { throw SettingsError(2, fmt::format("{}: {}", src, msg)); };
  const auto relative = [&](const json& v) {
    const std::filesystem::path p = pathFromUtf8(v.get<std::string>());
    return p.is_absolute() ? p : base / p;
  };

  s.scene_description = file;
  for (const auto& [key, value] : pen.items()) {
    if (key == "scene") {
      if (!value.is_string() || value.get<std::string>().empty()) fail("'scene' must be the path to a .glb or .gltf");
      s.scene = relative(value);
    } else if (key == "camera") {
      if (value.is_string()) {
        s.camera = value.get<std::string>();
        continue;
      }
      if (!value.is_object()) fail("'camera' must be a camera name or an object");
      CameraOverrides& co = s.camera_overrides;
      for (const auto& [ck, cv] : value.items()) {
        if (ck == "name" && cv.is_string()) {
          s.camera = cv.get<std::string>();
        } else if (ck == "projection" && cv.is_string() &&
                   (cv == "perspective" || cv == "orthographic" || cv == "equirectangular")) {
          co.projection = cv.get<std::string>();
        } else if (ck == "fstop" && cv.is_number() && cv.get<double>() >= 0) {
          co.fstop = cv.get<double>();
        } else if (ck == "focus_distance" && cv.is_number() && cv.get<double>() >= 0) {
          co.focus_distance = cv.get<double>();
        } else if (ck == "sensor_width_mm" && cv.is_number() && cv.get<double>() > 0) {
          co.sensor_width_mm = cv.get<double>();
        } else if (ck == "name" || ck == "projection" || ck == "fstop" || ck == "focus_distance" ||
                   ck == "sensor_width_mm") {
          fail(fmt::format("camera.{} has an invalid value {}", ck, cv.dump()));
        } else {
          warnings.push_back(fmt::format("{}: unknown key 'camera.{}' (ignored)", src, ck));
        }
      }
    } else if (key == "environment") {
      if (!value.is_object()) fail("'environment' must be an object");
      for (const auto& [ek, ev] : value.items()) {
        if (ek == "hdri" && ev.is_string()) {
          s.environment.hdri = relative(ev);
        } else if (ek == "color" && ev.is_array() && ev.size() == 3 &&
                   std::all_of(ev.begin(), ev.end(), [](const json& c) { return c.is_number() && c.get<double>() >= 0; })) {
          for (int c = 0; c < 3; ++c) s.environment.color[c] = ev[c].get<double>();
        } else if (ek == "strength" && ev.is_number() && ev.get<double>() >= 0) {
          s.environment.strength = ev.get<double>();
        } else if (ek == "rotation_deg" && ev.is_number()) {
          s.environment.rotation_deg = ev.get<double>();
        } else if (ek == "hdri" || ek == "color" || ek == "strength" || ek == "rotation_deg") {
          fail(fmt::format("environment.{} has an invalid value {}", ek, ev.dump()));
        } else {
          warnings.push_back(fmt::format("{}: unknown key 'environment.{}' (ignored)", src, ek));
        }
      }
    } else if (key == "sky" || key == "atmosphere") {
      if (!value.is_object()) fail(fmt::format("'{}' must be an object", key));
      // A number in [lo, hi] (hi exclusive when open), or a failure naming the key.
      const auto number = [&](const std::string& k, const json& v, double lo, double hi, bool open = false) {
        if (!v.is_number() || v.get<double>() < lo || (open ? v.get<double>() >= hi : v.get<double>() > hi))
          fail(fmt::format("{}.{} has an invalid value {} (expected {} to {})", key, k, v.dump(), lo, hi));
        return v.get<double>();
      };
      const double kBig = 1e12;
      if (key == "sky") {
        SkySettings& sky = s.sky;
        sky.enabled = true;
        for (const auto& [k, v] : value.items()) {
          if (k == "type") {
            if (v != "physical") fail(fmt::format("sky.type has an invalid value {} (expected \"physical\")", v.dump()));
          } else if (k == "sun_elevation_deg") sky.sun_elevation_deg = number(k, v, -90, 90);
          else if (k == "sun_azimuth_deg") sky.sun_azimuth_deg = number(k, v, -360, 360);
          else if (k == "sun_size_deg") sky.sun_size_deg = number(k, v, 0.01, 90);
          else if (k == "sun_strength") sky.sun_strength = number(k, v, 0, kBig);
          else if (k == "strength") sky.strength = number(k, v, 0, kBig);
          else if (k == "altitude") sky.altitude = number(k, v, -500, 60000);
          else if (k == "air") sky.air = number(k, v, 0, 10);
          else if (k == "dust") sky.dust = number(k, v, 0, 10);
          else if (k == "ozone") sky.ozone = number(k, v, 0, 10);
          else if (k == "resolution") sky.resolution = static_cast<uint32_t>(number(k, v, 64, 16384));
          else if (k == "sun_disc") {
            if (!v.is_boolean()) fail(fmt::format("sky.sun_disc has an invalid value {} (expected true or false)", v.dump()));
            sky.sun_disc = v.get<bool>();
          } else {
            warnings.push_back(fmt::format("{}: unknown key 'sky.{}' (ignored)", src, k));
          }
        }
      } else {
        AtmosphereSettings& a = s.atmosphere;
        a.enabled = true;
        for (const auto& [k, v] : value.items()) {
          if (k == "air") a.air = number(k, v, 0, 1000);
          else if (k == "haze") a.haze = number(k, v, 0, 1000);
          else if (k == "haze_anisotropy") a.haze_anisotropy = number(k, v, -0.99, 0.99);
          else if (k == "altitude") a.altitude = number(k, v, -500, 60000);
          else if (k == "meters_per_unit") a.meters_per_unit = number(k, v, 1e-6, 1e6);
          else if (k == "fog_density") a.fog_density = number(k, v, 0, 1000);
          else if (k == "fog_height") a.fog_height = number(k, v, 0.01, 1e6);
          else if (k == "fog_base") a.fog_base = number(k, v, -kBig, kBig);
          else if (k == "fog_anisotropy") a.fog_anisotropy = number(k, v, -0.99, 0.99);
          else if (k == "fog_color") {
            if (!v.is_array() || v.size() != 3 ||
                !std::all_of(v.begin(), v.end(), [](const json& c) { return c.is_number() && c.get<double>() >= 0 && c.get<double>() <= 1; }))
              fail(fmt::format("atmosphere.fog_color has an invalid value {} (expected [r, g, b], each 0 to 1)", v.dump()));
            for (int c = 0; c < 3; ++c) a.fog_color[c] = v[c].get<double>();
          } else {
            warnings.push_back(fmt::format("{}: unknown key 'atmosphere.{}' (ignored)", src, k));
          }
        }
      }
    } else if (key == "render") {
      if (!value.is_object()) fail("'render' must be an object");
      applyLayer(s, value, Layer::SceneDescription, src + " render", warnings);
    } else {
      warnings.push_back(fmt::format("{}: unknown key '{}' (ignored)", src, key));
    }
  }
  if (s.scene.empty()) fail("'scene' is missing");
}

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

ResolvedSettings resolveSettings(const SettingsSources& sources) {
  ResolvedSettings out;
  RenderSettings& s = out.settings;

  // 1. User config file.
  std::filesystem::path cfg;
  if (sources.config_file) cfg = *sources.config_file;
  else if (sources.load_user_config) cfg = paths::userConfigFile();
  if (!cfg.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(cfg, ec)) {
      applyLayer(s, readJsonObject(cfg, 1, "config file"), Layer::ConfigFile,
                 fmt::format("config file {}", pathToUtf8(cfg)), out.warnings);
      out.config_file = cfg;
    } else if (sources.config_file) {
      throw SettingsError(1, fmt::format("--config: file not found: {}", pathToUtf8(cfg)));
    }
  }

  // 2. Scene description (.pen.json), which carries its own render settings.
  json cli = sources.cli;
  if (auto it = cli.find("scene"); it != cli.end()) {
    if (!it->is_string() || it->get<std::string>().empty()) throw SettingsError(1, "--scene: empty path");
    const std::filesystem::path scene = pathFromUtf8(it->get<std::string>());
    if (endsWith(toLower(pathToUtf8(scene.filename())), ".pen.json")) loadSceneDescription(s, scene, out.warnings);
    else s.scene = scene;
    cli.erase(it);
  }

  // 3. Command line wins.
  applyLayer(s, cli, Layer::CommandLine, "command line", out.warnings);
  finalizeSettings(s);

  if (!s.scene.empty()) {
    const std::string ext = toLower(pathToUtf8(s.scene.extension()));
    if (ext != ".glb" && ext != ".gltf" && ext != ".obj")
      throw SettingsError(2, fmt::format("unsupported scene file '{}': use .glb, .gltf, .obj or .pen.json",
                                         pathToUtf8(s.scene)));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(s.scene, ec))
      throw SettingsError(2, fmt::format("scene file not found: {}", pathToUtf8(s.scene)));
  }
  return out;
}

}  // namespace pen
