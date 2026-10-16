#include "apps/penumbra/modes/info.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

#include "core/settings/parse.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"

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
  return ec ? "free space unknown" : std::format("{} free", formatBytes(space.available));
}

std::string existsNote(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) ? "exists" : "not created yet";
}

}  // namespace

int printVersion() {
  print(std::format("{}\nprotocol {}\n", versionString(), kProtocolVersion));
  return 0;
}

int listDevices() {
  const SystemInfo sys = querySystem();
  print(std::format("Render devices on {}:\n", sys.host));
  print(std::format("  cpu   {}, {} threads   backend cpu (Embree, from M1)\n", sys.cpu, sys.logical_cores));
  print("GPU detection is built with the GPU backends: Vulkan RT in M5, CUDA in M6.\n");
  return 0;
}

int diagnose() {
  const SystemInfo sys = querySystem();
  const auto row = [](std::string_view k, const std::string& v) { print(std::format("{:<13}{}\n", k, v)); };
  row("Version", versionString());
  row("Protocol", std::to_string(kProtocolVersion));
  row("OS", sys.os);
  row("Host", sys.host);
  row("CPU", std::format("{}, {} logical cores", sys.cpu, sys.logical_cores));
  row("RAM", std::format("{} total, {} available", formatBytes(sys.ram_total), formatBytes(sys.ram_available)));
  const auto cfg = paths::userConfigFile();
  row("Config", std::format("{} ({})", pathToUtf8(cfg), existsNote(cfg)));
  const auto logs = paths::logDir();
  row("Logs", std::format("{} ({})", pathToUtf8(logs), freeSpace(logs)));
  const auto cache = paths::sceneCacheDir();
  row("Scene cache", std::format("{} ({})", pathToUtf8(cache), freeSpace(cache)));
  row("User data", pathToUtf8(paths::userDataDir()));
  row("Film", std::format("output folder by default ({} in the current folder)", freeSpace(std::filesystem::current_path())));
  row("Vulkan", "not probed yet (Vulkan RT backend arrives in M5)");
  row("CUDA", "not probed yet (CUDA backend arrives in M6)");
  row("TCP 7420", "bind check arrives with networking in M4");
  return 0;
}

}  // namespace pen::app
