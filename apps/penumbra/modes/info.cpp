#include "apps/penumbra/modes/info.h"

#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <string>

#include "core/settings/parse.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"
#if defined(PEN_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.h"
#endif

namespace pen::app {

namespace {

void print(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); }

// Free space on the drive holding p (or its nearest existing parent).
std::string freeSpace(std::filesystem::path p) {
  std::error_code ec;
  while (!p.empty() && !std::filesystem::exists(p, ec)) {
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  const auto space = std::filesystem::space(p, ec);
  return ec ? "free space unknown" : fmt::format("{} free", formatBytes(space.available));
}

std::string existsNote(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) ? "exists" : "not created yet";
}

}  // namespace

int printVersion() {
  print(fmt::format("{}\nprotocol {}\n", versionString(), kProtocolVersion));
  return 0;
}

int listDevices() {
  const SystemInfo sys = querySystem();
  print(fmt::format("Render devices on {} (numbers are what --devices takes):\n", sys.host));
#if defined(PEN_HAS_VULKAN)
  const VulkanProbe& probe = probeVulkan();
  uint32_t n = 0;
  for (const VulkanGpu& g : probe.gpus) {
    if (g.ray_tracing)
      print(fmt::format("  {:<5} {}, {} VRAM   backend vulkan (hardware ray tracing)\n", n++, g.name,
                        formatBytes(g.vram_bytes)));
  }
  for (const VulkanGpu& g : probe.gpus)
    if (!g.ray_tracing)
      print(fmt::format("  -     {} ({}): not used, no Vulkan ray tracing (missing {})\n", g.name, g.type, g.missing));
  if (!probe.loader_found) print(fmt::format("  -     no GPU: {}\n", probe.error));
#else
  print("  -     GPUs: this build has no GPU backend (PEN_ENABLE_VULKAN=OFF)\n");
#endif
  print(fmt::format("  cpu   {}, {} threads   backend cpu (Embree)\n", sys.cpu, sys.logical_cores));
  print("CUDA (GPUs without ray tracing) arrives in M6.\n");
  return 0;
}

int diagnose() {
  const SystemInfo sys = querySystem();
  const auto row = [](std::string_view k, const std::string& v) { print(fmt::format("{:<13}{}\n", k, v)); };
  row("Version", versionString());
  row("Protocol", std::to_string(kProtocolVersion));
  row("OS", sys.os);
  row("Host", sys.host);
  row("CPU", fmt::format("{}, {} logical cores", sys.cpu, sys.logical_cores));
  row("RAM", fmt::format("{} total, {} available", formatBytes(sys.ram_total), formatBytes(sys.ram_available)));
  const auto cfg = paths::userConfigFile();
  row("Config", fmt::format("{} ({})", pathToUtf8(cfg), existsNote(cfg)));
  const auto logs = paths::logDir();
  row("Logs", fmt::format("{} ({})", pathToUtf8(logs), freeSpace(logs)));
  const auto cache = paths::sceneCacheDir();
  row("Scene cache", fmt::format("{} ({})", pathToUtf8(cache), freeSpace(cache)));
  row("User data", pathToUtf8(paths::userDataDir()));
  row("Film", fmt::format("output folder by default ({} in the current folder)", freeSpace(std::filesystem::current_path())));
#if defined(PEN_HAS_VULKAN)
  const VulkanProbe& probe = probeVulkan();
  if (!probe.loader_found) row("Vulkan", probe.error);
  for (const VulkanGpu& g : probe.gpus)
    row("Vulkan", fmt::format("{} ({}, {} VRAM, driver {}): {}", g.name, g.type, formatBytes(g.vram_bytes), g.driver,
                              g.ray_tracing ? "ray tracing OK" : "no ray tracing, missing " + g.missing));
#else
  row("Vulkan", "not built (PEN_ENABLE_VULKAN=OFF)");
#endif
  row("CUDA", "not probed yet (CUDA backend arrives in M6)");
  row("TCP 7420", "bind check arrives with networking in M4");
  return 0;
}

}  // namespace pen::app
