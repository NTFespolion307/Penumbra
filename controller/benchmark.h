#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace pen {

struct BenchmarkOptions {
  nlohmann::json cli = nlohmann::json::object();  // settings given on the command line (scene, --res, --devices...)
  std::optional<std::filesystem::path> config_file;
  bool load_user_config = true;
  std::filesystem::path output;  // the JSON report; the Markdown table goes next to it (.md)
};

// `penumbra --benchmark` (M9): renders each scene on each selected device alone, through the normal controller ->
// scheduler -> local worker -> film path, and reports per device and scene: samples/s while rendering, samples/s
// over its render window, busy % (the render window excludes the scene upload and build), the upload and build
// time and the memory tier. The scene given, else the repo suite in ./scenes (cornell, spheres, sponza,
// intel-sponza; scenes not downloaded are skipped). Defaults: 1080p, --time-limit 30s. Returns the exit code.
int runBenchmark(const BenchmarkOptions& options);

}  // namespace pen
