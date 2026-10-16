#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pen {

enum class Integrator { Path, Ao, DebugNormal, DebugAlbedo, DebugDepth, DebugBvh };
enum class BackendKind { Auto, Vulkan, Cuda, Cpu, Optix };
enum class ImageFormat { Png, Exr, Jpeg, Tiff, Webp };
enum class Denoiser { None, Oidn, Optix };
enum class Tonemap { Agx, Filmic, Aces, None };
enum class ProgressStyle { Bar, Plain, Json };
enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

// Name tables shared by the CLI, .pen.json, the config file and logs.
template <class E>
struct EnumNames;

#define PEN_ENUM_NAMES(E, N, ...)                                                    \
  template <>                                                                        \
  struct EnumNames<E> {                                                              \
    static constexpr std::array<std::pair<E, std::string_view>, N> items{{__VA_ARGS__}}; \
  };

PEN_ENUM_NAMES(Integrator, 6, {Integrator::Path, "path"}, {Integrator::Ao, "ao"},
               {Integrator::DebugNormal, "debug:normal"}, {Integrator::DebugAlbedo, "debug:albedo"},
               {Integrator::DebugDepth, "debug:depth"}, {Integrator::DebugBvh, "debug:bvh"})
PEN_ENUM_NAMES(BackendKind, 5, {BackendKind::Auto, "auto"}, {BackendKind::Vulkan, "vulkan"},
               {BackendKind::Cuda, "cuda"}, {BackendKind::Cpu, "cpu"}, {BackendKind::Optix, "optix"})
PEN_ENUM_NAMES(ImageFormat, 5, {ImageFormat::Png, "png"}, {ImageFormat::Exr, "exr"}, {ImageFormat::Jpeg, "jpg"},
               {ImageFormat::Tiff, "tiff"}, {ImageFormat::Webp, "webp"})
PEN_ENUM_NAMES(Denoiser, 3, {Denoiser::None, "none"}, {Denoiser::Oidn, "oidn"}, {Denoiser::Optix, "optix"})
PEN_ENUM_NAMES(Tonemap, 4, {Tonemap::Agx, "agx"}, {Tonemap::Filmic, "filmic"}, {Tonemap::Aces, "aces"},
               {Tonemap::None, "none"})
PEN_ENUM_NAMES(ProgressStyle, 3, {ProgressStyle::Bar, "bar"}, {ProgressStyle::Plain, "plain"},
               {ProgressStyle::Json, "json"})
PEN_ENUM_NAMES(LogLevel, 6, {LogLevel::Trace, "trace"}, {LogLevel::Debug, "debug"}, {LogLevel::Info, "info"},
               {LogLevel::Warn, "warn"}, {LogLevel::Error, "error"}, {LogLevel::Off, "off"})
#undef PEN_ENUM_NAMES

template <class E>
constexpr std::string_view toString(E value) {
  for (const auto& [v, name] : EnumNames<E>::items)
    if (v == value) return name;
  return "?";
}

template <class E>
constexpr std::optional<E> enumFromString(std::string_view text) {
  for (const auto& [v, name] : EnumNames<E>::items)
    if (name == text) return v;
  return std::nullopt;
}

struct Resolution {
  uint32_t width = 1920;
  uint32_t height = 1080;
  std::string preset = "1080p";  // empty for a custom WxH
  bool operator==(const Resolution&) const = default;
};

// Pixel rectangle, half-open: x0 <= x < x1, y0 <= y < y1.
struct PixelRect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool operator==(const PixelRect&) const = default;
};

// Sample indices, inclusive on both ends ("0-63" is 64 samples).
struct SampleRange {
  uint32_t first = 0, last = 0;
  bool operator==(const SampleRange&) const = default;
};

struct EnvironmentSettings {
  std::filesystem::path hdri;  // empty: no HDRI
  double strength = 1.0;
  double rotation_deg = 0.0;
  bool operator==(const EnvironmentSettings&) const = default;
};

// Every render-affecting or persistent setting. The CLI, .pen.json "render" section and the user config
// file all write into this one struct through core/settings/schema.h. Field names equal the JSON keys.
struct RenderSettings {
  // General
  LogLevel log_level = LogLevel::Info;
  std::filesystem::path log_file;  // empty: per-OS log folder
  ProgressStyle progress = ProgressStyle::Bar;

  // Scene
  std::filesystem::path scene;              // geometry file actually loaded (.glb, .gltf, .obj)
  std::filesystem::path scene_description;  // the .pen.json it came from, if any
  std::string camera;                       // empty: first camera in the scene
  EnvironmentSettings environment;

  // Render
  Resolution res;
  double scale = 1.0;
  uint32_t spp = 128;  // 0 only together with time_limit
  uint32_t max_bounces = 12;
  std::optional<uint32_t> max_diffuse, max_glossy, max_transmission;  // unset: max_bounces
  Integrator integrator = Integrator::Path;
  bool no_rr = false;
  double ao_distance = 0.0;  // 0: automatic
  uint32_t seed = 0;
  uint64_t time_limit_s = 0;  // 0: none

  // Devices
  BackendKind backend = BackendKind::Auto;
  std::string devices = "all";  // "all", "gpu", or a list like "0,1,cpu"
  uint32_t threads = 0;         // 0: all cores
  uint32_t tile_size = 256;
  uint64_t vram_limit = 0;  // bytes, 0: automatic
  uint64_t ram_limit = 0;   // bytes, 0: automatic (75% of RAM)
  bool no_texture_downsize = false;

  // Output
  std::filesystem::path output = "render.png";
  std::optional<ImageFormat> format;  // unset: from the output extension; always set after finalize
  uint32_t bit_depth = 0;             // 0: format default; always set after finalize
  Denoiser denoise = Denoiser::None;
  Tonemap tonemap = Tonemap::Agx;
  double exposure = 0.0;
  std::vector<std::string> aov;

  // Film
  std::filesystem::path film_dir;  // empty: output folder
  uint64_t checkpoint_s = 600;
  std::optional<PixelRect> region;
  std::optional<SampleRange> sample_range;

  // Controller
  uint16_t port = 7420;
  std::string bind;  // empty: all interfaces
  bool approve_workers = false;
  uint32_t max_workers = 64;
  bool no_upnp = false;
  bool no_local = false;
  uint64_t upload_limit = 0;  // bytes per second, 0: unlimited

  // Worker
  std::string worker_name;  // empty: host name
  bool stay = false;
  std::filesystem::path cache_dir;  // empty: per-OS cache folder
  uint64_t cache_size = 20ull << 30;

  // Final image size after --scale (at least 1x1).
  uint32_t outputWidth() const;
  uint32_t outputHeight() const;

  bool operator==(const RenderSettings&) const = default;
};

}  // namespace pen
