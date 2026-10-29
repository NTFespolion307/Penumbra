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
enum class BackendKind { Auto, Vulkan, Cuda, Cpu, Optix, Null };  // Null: test-only (D-047)
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
PEN_ENUM_NAMES(BackendKind, 6, {BackendKind::Auto, "auto"}, {BackendKind::Vulkan, "vulkan"},
               {BackendKind::Cuda, "cuda"}, {BackendKind::Cpu, "cpu"}, {BackendKind::Optix, "optix"},
               {BackendKind::Null, "null"})
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
  std::array<double, 3> color{0, 0, 0};  // uniform radiance when there is no HDRI
  double strength = 1.0;
  double rotation_deg = 0.0;
  bool operator==(const EnvironmentSettings&) const = default;
};

// A physical sky from the sun direction (.pen.json "sky", M14): Nishita single scattering (Rayleigh, Mie, ozone),
// like Blender's Nishita sky, baked at load into the environment map (it replaces "environment"). Angles in
// degrees; the azimuth turns from -Z (glTF forward) towards +X; densities are multiples of Earth's.
struct SkySettings {
  bool enabled = false;
  double sun_elevation_deg = 30;
  double sun_azimuth_deg = 0;
  double sun_size_deg = 0.545;  // angular diameter
  double sun_strength = 1;      // the sun disc only (as Blender's Sun Intensity)
  bool sun_disc = true;         // false: the disc is not drawn, the sky still lit by it
  double strength = 1;          // the whole sky
  double altitude = 0;          // metres above sea level of the viewer
  double air = 1, dust = 1, ozone = 1;
  uint32_t resolution = 2048;   // width of the baked map (height half)
  bool operator==(const SkySettings&) const = default;
};

// A participating medium filling the scene (.pen.json "atmosphere", M14): exponential height layers, so
// transmittance has a closed form. Air (Rayleigh) and haze (Mie) are Earth's (as in the physical sky, whose
// densities they follow when unset); fog is a user layer. Heights: scene up axis (+Y), in metres via
// meters_per_unit; altitude is the sea-level height of y = 0.
struct AtmosphereSettings {
  bool enabled = false;
  std::optional<double> air, haze;  // unset: the sky's air and dust (1 without a sky)
  double haze_anisotropy = 0.76;     // Henyey-Greenstein g of the haze
  double altitude = 0;
  double meters_per_unit = 1;
  double fog_density = 0;            // extinction per metre at fog_base (0: no fog)
  double fog_height = 100;           // scale height, metres
  double fog_base = 0;               // scene units (y)
  double fog_anisotropy = 0;
  std::array<double, 3> fog_color{1, 1, 1};  // single-scattering albedo
  bool operator==(const AtmosphereSettings&) const = default;
};

// Camera settings glTF cannot carry, from the .pen.json "camera" object.
struct CameraOverrides {
  std::string projection;       // empty: from the scene; "perspective", "orthographic", "equirectangular"
  double fstop = 0;             // 0: pinhole (no depth of field)
  double focus_distance = 0;    // scene units; 0: distance to the scene centre
  double sensor_width_mm = 36;  // for converting f-stop to an aperture
  bool operator==(const CameraOverrides&) const = default;
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
  CameraOverrides camera_overrides;
  EnvironmentSettings environment;
  SkySettings sky;
  AtmosphereSettings atmosphere;

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
  std::string devices = "all";  // "all", "gpu", or a list like "0,1:cuda,cpu" (parseDeviceList)
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

// One entry of a --devices list: a GPU number (as penumbra --list-devices shows it) with an optional backend
// ("1:cuda"), or "cpu". The same GPU may appear twice with different backends: two slots on one GPU (DECISIONS
// D-090). Throws ValueError on anything else; "all" and "gpu" are not lists.
struct DeviceItem {
  bool cpu = false;
  uint32_t gpu = 0;
  BackendKind backend = BackendKind::Auto;  // Vulkan or Cuda when the item names one
};
std::vector<DeviceItem> parseDeviceList(std::string_view list);

}  // namespace pen
