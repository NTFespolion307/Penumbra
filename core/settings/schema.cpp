#include "core/settings/schema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/settings/parse.h"
#include "core/system/paths.h"

namespace pen {

using nlohmann::json;

uint32_t RenderSettings::outputWidth() const {
  return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(res.width * scale)));
}
uint32_t RenderSettings::outputHeight() const {
  return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(res.height * scale)));
}

namespace {

// ---- JSON value helpers. CLI values arrive as strings, file values as native JSON types. ----

std::string text(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number()) return v.dump();
  throw ValueError(fmt::format("expected a value, got {}", v.dump()));
}

uint64_t asUint(const json& v, uint64_t lo, uint64_t hi) { return parseUint(text(v), lo, hi); }
uint32_t asU32(const json& v, uint64_t lo, uint64_t hi) { return static_cast<uint32_t>(asUint(v, lo, hi)); }
double asDouble(const json& v, double lo, double hi) { return parseDouble(text(v), lo, hi); }
bool asBool(const json& v) { return v.is_boolean() ? v.get<bool>() : parseBool(text(v)); }
std::filesystem::path asPath(const json& v) { return pathFromUtf8(text(v)); }

template <class E>
E asEnum(const json& v) {
  const std::string t = toLower(trim(text(v)));
  if (auto e = enumFromString<E>(t)) return *e;
  std::string choices;
  for (const auto& [value, name] : EnumNames<E>::items) choices += (choices.empty() ? "" : ", ") + std::string(name);
  throw ValueError(fmt::format("'{}' is not one of: {}", t, choices));
}

bool isWord(const json& v, std::string_view word) { return v.is_string() && toLower(trim(text(v))) == word; }

std::optional<uint32_t> asOptionalBounces(const json& v) {
  if (v.is_null() || isWord(v, "auto")) return std::nullopt;
  return asU32(v, 0, 1024);
}

std::optional<ImageFormat> formatFromName(std::string_view name) {
  const std::string n = toLower(name);
  if (n == "jpeg") return ImageFormat::Jpeg;
  if (n == "tif") return ImageFormat::Tiff;
  return enumFromString<ImageFormat>(n);
}

std::string normalizeDevices(const json& v) {
  std::string t = toLower(text(v));
  std::erase_if(t, [](char c) { return c == ' '; });
  if (t == "all" || t == "gpu") return t;
  size_t start = 0;
  for (;;) {
    const size_t comma = t.find(',', start);
    const std::string_view item = std::string_view(t).substr(start, comma - start);
    if (item != "cpu") {
      try {
        parseUint(item, 0, 255);
      } catch (const ValueError&) {
        throw ValueError(fmt::format("'{}' is not 'all', 'gpu' or a list like '0,1,cpu'", text(v)));
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return t;
}

std::vector<std::string> parseAovs(const json& v) {
  std::vector<std::string> items;
  if (v.is_array()) {
    for (const auto& e : v) items.push_back(toLower(trim(text(e))));
  } else {
    const std::string t = text(v);
    size_t start = 0;
    for (;;) {
      const size_t comma = t.find(',', start);
      const std::string item = toLower(trim(std::string_view(t).substr(start, comma - start)));
      if (!item.empty() && item != "none") items.push_back(item);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  }
  std::vector<std::string> out;
  for (const auto& item : items) {
    if (item != "albedo" && item != "normal" && item != "depth")
      throw ValueError(fmt::format("'{}' is not an AOV (albedo, normal, depth)", item));
    if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
  }
  return out;
}

using S = RenderSettings;
using K = OptionKind;

// Keep groups contiguous and in --help order.
const std::array kSchema = {
    // ---- General ----
    SettingSpec{"log_level", "--log-level", "General", K::Value, "trace, debug, info, warn, error, off", "info", true,
                "--log-level debug", "Log verbosity for console and log file",
                [](S& s, const json& v) { s.log_level = asEnum<LogLevel>(v); }},
    SettingSpec{"log_file", "--log-file", "General", K::Value, "path", "per-OS log folder", false,
                "--log-file render.log", "Write the log here instead of the per-OS log folder",
                [](S& s, const json& v) { s.log_file = asPath(v); }},
    SettingSpec{"progress", "--progress", "General", K::Value, "bar, plain, json", "bar", true, "--progress json",
                "Progress output style; json prints one object per second on stdout",
                [](S& s, const json& v) { s.progress = asEnum<ProgressStyle>(v); }},
    SettingSpec{"scene", "--scene,scene", "General", K::Value, ".glb, .gltf, .obj, .pen.json", "none", false,
                "scene.glb", "Scene to load (also accepted as the first positional argument)",
                [](S& s, const json& v) { s.scene = asPath(v); }},

    // ---- Render ----
    SettingSpec{"res", "--res", "Render", K::Value, "120p, 240p, 360p, 480p, 720p, 1080p, 2k, 4k, 6k, 8k, 10k, 15k, "
                "20k, 40k, 60k, 80k, 160k, or WxH", "1080p", true, "--res 4k", "Output resolution",
                [](S& s, const json& v) { s.res = parseResolution(text(v)); }},
    SettingSpec{"scale", "--scale", "Render", K::Value, "0.01-1.0", "1.0", true, "--scale 0.25",
                "Render the resolution at a fraction of its size (quick tests)",
                [](S& s, const json& v) { s.scale = asDouble(v, 0.01, 1.0); }},
    SettingSpec{"spp", "--spp", "Render", K::Value, "1-4294967295 (0 with --time-limit)", "128", true, "--spp 1024",
                "Samples per pixel",
                [](S& s, const json& v) { s.spp = asU32(v, 0, 4294967295ull); }},
    SettingSpec{"max_bounces", "--max-bounces", "Render", K::Value, "0-1024", "12", true, "--max-bounces 32",
                "Path depth cap (0 = direct light only)",
                [](S& s, const json& v) { s.max_bounces = asU32(v, 0, 1024); }},
    SettingSpec{"max_diffuse", "--max-diffuse", "Render", K::Value, "0-1024", "--max-bounces", false,
                "--max-diffuse 4", "Diffuse bounce cap",
                [](S& s, const json& v) { s.max_diffuse = asOptionalBounces(v); }},
    SettingSpec{"max_glossy", "--max-glossy", "Render", K::Value, "0-1024", "--max-bounces", false,
                "--max-glossy 8", "Glossy bounce cap",
                [](S& s, const json& v) { s.max_glossy = asOptionalBounces(v); }},
    SettingSpec{"max_transmission", "--max-transmission", "Render", K::Value, "0-1024", "--max-bounces", false,
                "--max-transmission 16", "Transmission bounce cap",
                [](S& s, const json& v) { s.max_transmission = asOptionalBounces(v); }},
    SettingSpec{"integrator", "--integrator", "Render", K::Value,
                "path, ao, debug:normal, debug:albedo, debug:depth, debug:bvh", "path", true, "--integrator ao",
                "Light transport algorithm",
                [](S& s, const json& v) { s.integrator = asEnum<Integrator>(v); }},
    SettingSpec{"no_rr", "--no-rr", "Render", K::Flag, "", "off", false, "--no-rr",
                "Disable Russian roulette path termination",
                [](S& s, const json& v) { s.no_rr = asBool(v); }},
    SettingSpec{"ao_distance", "--ao-distance", "Render", K::Value, "scene units, 0 = automatic", "0", true,
                "--ao-distance 0.5", "Occlusion distance for --integrator ao",
                [](S& s, const json& v) { s.ao_distance = asDouble(v, 0.0, 1e30); }},
    SettingSpec{"seed", "--seed", "Render", K::Value, "0-4294967295", "0", true, "--seed 7",
                "Deterministic sampling seed",
                [](S& s, const json& v) { s.seed = asU32(v, 0, 4294967295ull); }},
    SettingSpec{"camera", "--camera", "Render", K::Value, "camera name", "first in scene", false,
                "--camera Camera.001", "Camera to render from",
                [](S& s, const json& v) { s.camera = text(v); }},
    SettingSpec{"time_limit", "--time-limit", "Render", K::Value, "duration (2h30m, 45m, 90s) or none", "none", true,
                "--time-limit 2h30m", "Stop and write the output at the deadline",
                [](S& s, const json& v) { s.time_limit_s = isWord(v, "none") ? 0 : parseDuration(text(v)); }},

    // ---- Devices ----
    SettingSpec{"backend", "--backend", "Devices", K::Value, "auto, vulkan, cuda, cpu, optix", "auto", true,
                "--backend cuda", "Ray tracing backend",
                [](S& s, const json& v) { s.backend = asEnum<BackendKind>(v); }},
    SettingSpec{"devices", "--devices", "Devices", K::Value, "all, gpu, or a list like 0,1,cpu", "all", true,
                "--devices 0,cpu", "Which local devices render (controller or worker)",
                [](S& s, const json& v) { s.devices = normalizeDevices(v); }},
    SettingSpec{"threads", "--threads", "Devices", K::Value, "0-4096, 0 = all cores", "0", true, "--threads 8",
                "CPU worker threads",
                [](S& s, const json& v) { s.threads = asU32(v, 0, 4096); }},
    SettingSpec{"tile_size", "--tile-size", "Devices", K::Value, "16-4096", "256", true, "--tile-size 128",
                "Work item tile size in pixels",
                [](S& s, const json& v) { s.tile_size = asU32(v, 16, 4096); }},
    SettingSpec{"vram_limit", "--vram-limit", "Devices", K::Value, "size (6G, 512M) or auto", "auto", true,
                "--vram-limit 6G", "VRAM budget per GPU (default: free VRAM minus 10%)",
                [](S& s, const json& v) { s.vram_limit = isWord(v, "auto") ? 0 : parseByteSize(text(v)); }},
    SettingSpec{"ram_limit", "--ram-limit", "Devices", K::Value, "size (24G) or auto", "auto", true,
                "--ram-limit 24G", "RAM budget (default: 75% of physical RAM)",
                [](S& s, const json& v) { s.ram_limit = isWord(v, "auto") ? 0 : parseByteSize(text(v)); }},
    SettingSpec{"no_texture_downsize", "--no-texture-downsize", "Devices", K::Flag, "", "off", false,
                "--no-texture-downsize", "Forbid memory tier 2 (downsized textures)",
                [](S& s, const json& v) { s.no_texture_downsize = asBool(v); }},

    // ---- Output ----
    SettingSpec{"output", "-o,--output", "Output", K::Value, "file path; the extension picks the format",
                "render.png", true, "-o out.exr", "Output image",
                [](S& s, const json& v) { s.output = asPath(v); }},
    SettingSpec{"format", "--format", "Output", K::Value, "png, exr, jpg, tiff, webp", "from extension", false,
                "--format exr", "Output encoding (must match the extension if there is one)",
                [](S& s, const json& v) {
                  if (v.is_null() || isWord(v, "auto")) {
                    s.format.reset();
                    return;
                  }
                  s.format = formatFromName(trim(text(v)));
                  if (!s.format) throw ValueError(fmt::format("'{}' is not one of: png, exr, jpg, tiff, webp", text(v)));
                }},
    SettingSpec{"bit_depth", "--bit-depth", "Output", K::Value, "8, 16, 32 (per format)", "png/jpg/webp 8, tiff/exr 16",
                false, "--bit-depth 32", "Bits per channel",
                [](S& s, const json& v) {
                  s.bit_depth = isWord(v, "auto") ? 0 : asU32(v, 8, 32);
                  if (s.bit_depth != 0 && s.bit_depth != 8 && s.bit_depth != 16 && s.bit_depth != 32)
                    throw ValueError(fmt::format("{} is not 8, 16 or 32", s.bit_depth));
                }},
    SettingSpec{"denoise", "--denoise", "Output", K::Value, "none, oidn, optix", "none", true, "--denoise oidn",
                "Denoiser",
                [](S& s, const json& v) { s.denoise = asEnum<Denoiser>(v); }},
    SettingSpec{"tonemap", "--tonemap", "Output", K::Value, "agx, filmic, aces, none", "agx", true,
                "--tonemap filmic", "Display transform",
                [](S& s, const json& v) { s.tonemap = asEnum<Tonemap>(v); }},
    SettingSpec{"exposure", "--exposure", "Output", K::Value, "EV, -20 to 20", "0", true, "--exposure 1.5",
                "Exposure adjustment before tonemapping",
                [](S& s, const json& v) { s.exposure = asDouble(v, -20.0, 20.0); }},
    SettingSpec{"aov", "--aov", "Output", K::Value, "albedo, normal, depth (comma list)", "none", true,
                "--aov albedo,normal", "Extra passes",
                [](S& s, const json& v) { s.aov = parseAovs(v); }},

    // ---- Film ----
    SettingSpec{"film_dir", "--film-dir", "Film", K::Value, "folder", "output folder", false, "--film-dir D:\\films",
                "Where the film and session file live",
                [](S& s, const json& v) { s.film_dir = asPath(v); }},
    SettingSpec{"checkpoint", "--checkpoint", "Film", K::Value, "duration, 10s to 7d", "10m", true,
                "--checkpoint 5m", "Checkpoint period",
                [](S& s, const json& v) {
                  const uint64_t d = parseDuration(text(v));
                  if (d < 10 || d > 7 * 86400) throw ValueError(fmt::format("{} is not between 10s and 7d", text(v)));
                  s.checkpoint_s = d;
                }},
    SettingSpec{"region", "--region", "Film", K::Value, "x0,y0,x1,y1 (x1, y1 exclusive) or none", "none", true,
                "--region 0,0,1024,1024", "Render only part of the image (for merge)",
                [](S& s, const json& v) {
                  if (v.is_null() || isWord(v, "none")) s.region.reset();
                  else s.region = parseRect(text(v));
                }},
    SettingSpec{"sample_range", "--sample-range", "Film", K::Value, "a-b (inclusive) or none", "none", true,
                "--sample-range 0-511", "Render only these samples (for merge)",
                [](S& s, const json& v) {
                  if (v.is_null() || isWord(v, "none")) s.sample_range.reset();
                  else s.sample_range = parseSampleRange(text(v));
                }},

    // ---- Controller ----
    SettingSpec{"port", "--port", "Controller", K::Value, "1-65535", "7420", true, "--port 7421", "Listen port",
                [](S& s, const json& v) { s.port = static_cast<uint16_t>(asUint(v, 1, 65535)); }},
    SettingSpec{"bind", "--bind", "Controller", K::Value, "address", "all interfaces", false, "--bind 192.168.1.20",
                "Listen address",
                [](S& s, const json& v) { s.bind = std::string(trim(text(v))); }},
    SettingSpec{"approve_workers", "--approve-workers", "Controller", K::Flag, "", "off", false, "--approve-workers",
                "Ask before accepting each new worker",
                [](S& s, const json& v) { s.approve_workers = asBool(v); }},
    SettingSpec{"max_workers", "--max-workers", "Controller", K::Value, "1-4096", "64", true, "--max-workers 8",
                "Connection limit",
                [](S& s, const json& v) { s.max_workers = asU32(v, 1, 4096); }},
    SettingSpec{"no_upnp", "--no-upnp", "Controller", K::Flag, "", "off", false, "--no-upnp", "Never try UPnP",
                [](S& s, const json& v) { s.no_upnp = asBool(v); }},
    SettingSpec{"no_local", "--no-local", "Controller", K::Flag, "", "off", false, "--no-local",
                "Coordinate only, render nothing on this machine",
                [](S& s, const json& v) { s.no_local = asBool(v); }},
    SettingSpec{"upload_limit", "--upload-limit", "Controller", K::Value, "bytes per second (10M) or unlimited",
                "unlimited", true, "--upload-limit 10M", "Cap scene upload bandwidth",
                [](S& s, const json& v) { s.upload_limit = isWord(v, "unlimited") ? 0 : parseByteSize(text(v)); }},

    // ---- Worker ----
    SettingSpec{"worker_name", "--worker-name", "Worker", K::Value, "text", "host name", false,
                "--worker-name gpu-server-1", "Name shown in the controller UI",
                [](S& s, const json& v) { s.worker_name = std::string(trim(text(v))); }},
    SettingSpec{"stay", "--stay", "Worker", K::Flag, "", "off", false, "--stay",
                "Keep serving renders after one completes and reconnect on its own",
                [](S& s, const json& v) { s.stay = asBool(v); }},
    SettingSpec{"cache_dir", "--cache-dir", "Worker", K::Value, "folder", "per-OS cache folder", false,
                "--cache-dir /data/penumbra", "Scene cache folder",
                [](S& s, const json& v) { s.cache_dir = asPath(v); }},
    SettingSpec{"cache_size", "--cache-size", "Worker", K::Value, "size, at least 1G", "20G", true,
                "--cache-size 100G", "Scene cache size before least-recently-used eviction",
                [](S& s, const json& v) {
                  const uint64_t b = parseByteSize(text(v));
                  if (b < (1ull << 30)) throw ValueError(fmt::format("'{}' is smaller than 1G", text(v)));
                  s.cache_size = b;
                }},
};

std::string_view canonicalExtension(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return ".png";
    case ImageFormat::Exr: return ".exr";
    case ImageFormat::Jpeg: return ".jpg";
    case ImageFormat::Tiff: return ".tiff";
    case ImageFormat::Webp: return ".webp";
  }
  return "";
}

std::string_view formatLabel(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Exr: return "EXR";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Tiff: return "TIFF";
    case ImageFormat::Webp: return "WebP";
  }
  return "";
}

}  // namespace

std::span<const SettingSpec> settingSchema() { return kSchema; }

const SettingSpec* findSetting(std::string_view key) {
  for (const auto& s : kSchema)
    if (s.key == key) return &s;
  return nullptr;
}

std::string_view longFlag(const SettingSpec& spec) {
  std::string_view n = spec.names;
  const size_t start = n.find("--");
  n = n.substr(start);
  return n.substr(0, n.find(','));
}

void applySetting(RenderSettings& s, const SettingSpec& spec, const json& value, std::string_view where) {
  try {
    spec.apply(s, value);
  } catch (const ValueError& e) {
    throw SettingsError(1, fmt::format("{}: {}", where, e.what()));
  } catch (const json::exception& e) {
    throw SettingsError(1, fmt::format("{}: {}", where, e.what()));
  }
}

uint32_t formatMaxSide(ImageFormat f) {
  switch (f) {
    case ImageFormat::Jpeg: return 65500;  // libjpeg-turbo JPEG_MAX_DIMENSION
    case ImageFormat::Webp: return 16383;  // WEBP_MAX_DIMENSION
    default: return 0;
  }
}

void finalizeSettings(RenderSettings& s) {
  const auto fail = [](const std::string& msg) { throw SettingsError(1, msg); };

  if (s.spp == 0 && s.time_limit_s == 0)
    fail("--spp 0 means \"render until the time limit\" and needs --time-limit (for example --time-limit 2h)");

  if (s.output.empty()) fail("--output is empty");
  const std::string ext = toLower(pathToUtf8(s.output.extension()));
  const std::optional<ImageFormat> fromExt = ext.empty() ? std::nullopt : formatFromName(ext.substr(1));
  if (s.format) {
    if (ext.empty()) {
      s.output += std::string(canonicalExtension(*s.format));
    } else if (fromExt != s.format) {
      fail(fmt::format("--format {} does not match the output file '{}'", toString(*s.format), pathToUtf8(s.output)));
    }
  } else {
    if (!fromExt)
      fail(fmt::format("cannot tell the image format from '{}': use .png, .exr, .jpg, .tiff or .webp, or set --format",
                       pathToUtf8(s.output)));
    s.format = fromExt;
  }

  const ImageFormat f = *s.format;
  if (s.bit_depth == 0) {
    s.bit_depth = (f == ImageFormat::Exr || f == ImageFormat::Tiff) ? 16 : 8;
  } else {
    const bool ok = (f == ImageFormat::Png && (s.bit_depth == 8 || s.bit_depth == 16)) ||
                    (f == ImageFormat::Exr && (s.bit_depth == 16 || s.bit_depth == 32)) ||
                    ((f == ImageFormat::Jpeg || f == ImageFormat::Webp) && s.bit_depth == 8) ||
                    f == ImageFormat::Tiff;
    if (!ok)
      fail(fmt::format("{} does not support --bit-depth {} (PNG 8/16, EXR 16/32, JPEG 8, WebP 8, TIFF 8/16/32)",
                       formatLabel(f), s.bit_depth));
  }

  const uint32_t w = s.outputWidth(), h = s.outputHeight();
  if (const uint32_t maxSide = formatMaxSide(f); maxSide != 0 && (w > maxSide || h > maxSide)) {
    const std::string res = s.res.preset.empty() ? fmt::format("{}x{}", s.res.width, s.res.height) : s.res.preset;
    fail(fmt::format("{} images are limited to {} px per side, but {} is {}x{}. Use EXR, PNG or TIFF, or a smaller "
                     "--res or --scale.",
                     formatLabel(f), maxSide, res, w, h));
  }

  if (s.region && (s.region->x1 > w || s.region->y1 > h))
    fail(fmt::format("--region {},{},{},{} is outside the {}x{} image", s.region->x0, s.region->y0, s.region->x1,
                     s.region->y1, w, h));

  if (s.sample_range && s.spp != 0 && s.sample_range->last >= s.spp)
    fail(fmt::format("--sample-range {}-{} goes past --spp {} (the last sample is {})", s.sample_range->first,
                     s.sample_range->last, s.spp, s.spp - 1));
}

}  // namespace pen
