#include "backends/gpu/nvml.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pen {

namespace {

// The few NVML entry points Penumbra uses, declared here (their ABI is stable) so no NVML header is needed.
using NvmlReturn = int;  // 0 = NVML_SUCCESS
using NvmlDevice = struct NvmlDeviceOpaque*;
struct NvmlUtilization {
  unsigned int gpu, memory;
};
struct NvmlMemory {
  unsigned long long total, free, used;
};
constexpr int kNvmlTemperatureGpu = 0;

struct Nvml {
  NvmlReturn (*init)() = nullptr;
  NvmlReturn (*handleByUuid)(const char*, NvmlDevice*) = nullptr;
  NvmlReturn (*utilization)(NvmlDevice, NvmlUtilization*) = nullptr;
  NvmlReturn (*memory)(NvmlDevice, NvmlMemory*) = nullptr;
  NvmlReturn (*temperature)(NvmlDevice, int, unsigned int*) = nullptr;
  bool ok = false;
};

#if defined(_WIN32)
void* openNvml() { return reinterpret_cast<void*>(LoadLibraryA("nvml.dll")); }
void* nvmlSymbol(void* lib, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
void* openNvml() { return dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL); }
void* nvmlSymbol(void* lib, const char* name) { return dlsym(lib, name); }
#endif

const Nvml& nvml() {
  static const Nvml n = [] {
    Nvml r;
    void* lib = openNvml();
    if (!lib) {
      spdlog::debug("NVML not found: GPU usage, VRAM and temperature readings are unavailable");
      return r;
    }
    const auto load = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(nvmlSymbol(lib, name));
      return fn != nullptr;
    };
    if (!load(r.init, "nvmlInit_v2") || !load(r.handleByUuid, "nvmlDeviceGetHandleByUUID") ||
        !load(r.utilization, "nvmlDeviceGetUtilizationRates") || !load(r.memory, "nvmlDeviceGetMemoryInfo") ||
        !load(r.temperature, "nvmlDeviceGetTemperature")) {
      spdlog::debug("NVML lacks an entry point Penumbra needs; GPU readings are unavailable");
      return r;
    }
    const NvmlReturn rc = r.init();
    if (rc != 0) {
      spdlog::debug("NVML did not start (error {}); GPU readings are unavailable", rc);
      return r;
    }
    r.ok = true;  // the library stays loaded and initialised for the life of the process
    return r;
  }();
  return n;
}

}  // namespace

std::optional<GpuLiveStats> queryGpuLiveStats(const std::array<uint8_t, 16>& uuid) {
  if (uuid == std::array<uint8_t, 16>{}) return std::nullopt;
  const Nvml& n = nvml();
  if (!n.ok) return std::nullopt;
  const auto& u = uuid;
  const std::string text = fmt::format(
      "GPU-{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", u[0],
      u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
  NvmlDevice dev = nullptr;
  if (n.handleByUuid(text.c_str(), &dev) != 0) return std::nullopt;  // not an NVIDIA GPU
  GpuLiveStats s;
  NvmlUtilization util{};
  if (n.utilization(dev, &util) == 0) s.usage_percent = static_cast<float>(util.gpu);
  NvmlMemory mem{};
  if (n.memory(dev, &mem) == 0) {
    s.vram_used = mem.used;
    s.vram_total = mem.total;
  }
  unsigned int t = 0;
  if (n.temperature(dev, kNvmlTemperatureGpu, &t) == 0) s.temperature_c = static_cast<float>(t);
  return s;
}

}  // namespace pen
