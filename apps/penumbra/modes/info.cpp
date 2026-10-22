#include "apps/penumbra/modes/info.h"

#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <string>

#include "core/settings/parse.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"
#include "worker/device_manager.h"
#if defined(PEN_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.h"
#endif
#if defined(PEN_HAS_CUDA)
#include "backends/cuda/cuda_backend.h"
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
  const GpuInventory& inv = gpuInventory();
  for (const RenderGpu& g : inv.gpus) {
    std::string backends;
    if (g.vulkan >= 0) backends = "vulkan (hardware ray tracing, the default)";
    if (g.cuda >= 0) backends += g.vulkan >= 0 ? ", cuda" : "cuda (software BVH, the default)";
    print(fmt::format("  {:<5} {}, {} VRAM   backend {}\n", g.index, g.name, formatBytes(g.vram_bytes), backends));
  }
  print(fmt::format("  cpu   {}, {} threads   backend cpu (Embree)\n", sys.cpu, usableCpuThreads()));
  for (const std::string& n : inv.notes) print(fmt::format("  -     {}\n", n));
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
#if defined(PEN_HAS_CUDA)
  row("CUDA build", cudaBuildSummary());
  const CudaProbe& cuda = probeCuda();
  if (!cuda.driver_found) row("CUDA", cuda.error);
  else row("CUDA", fmt::format("driver supports CUDA {}.{}", cuda.driver_version / 1000, cuda.driver_version % 1000 / 10));
  for (const CudaGpu& g : cuda.gpus)
    row("CUDA", fmt::format("{} (compute {}.{}, {} VRAM): {}", g.name, g.cc_major, g.cc_minor,
                            formatBytes(g.vram_bytes), g.usable ? "OK" : g.why));
#else
  row("CUDA", "not built (no CUDA toolkit when Penumbra was built, or PEN_ENABLE_CUDA=OFF)");
#endif
  row("TCP 7420", "bind check arrives with networking in M4");
  return 0;
}

}  // namespace pen::app
