#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/settings/render_settings.h"

namespace pen {

// <name>.session.json next to the film (docs/GUIDE.md "Session file"): everything --resume needs. Written
// atomically at every checkpoint.
struct Session {
  std::string app_version, git_hash;
  RenderSettings settings;
  std::filesystem::path film;
  uint64_t scene_hash = 0, settings_hash = 0;
  std::string state = "rendering";  // rendering, interrupted, out_of_disk, time_limit, complete
  uint32_t tiles_done = 0, tiles_total = 0;
  uint64_t samples_done = 0, samples_total = 0;
  std::string created, updated;  // local time, ISO 8601

  void save(const std::filesystem::path& path) const;
  static Session load(const std::filesystem::path& path);  // throws SettingsError(1) on a bad file
};

// <film_dir or output folder>/<output stem>.penfilm and .session.json.
std::filesystem::path filmPathFor(const RenderSettings& s);
std::filesystem::path sessionPathFor(const std::filesystem::path& filmPath);

// Fingerprint of the files the image depends on: the scene file, the .pen.json and the HDRI. (M4 replaces it
// with the scene package hash, which also covers external buffers and textures.)
uint64_t sceneFingerprint(const RenderSettings& s);

std::string localTimestamp();

}  // namespace pen
