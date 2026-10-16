#include "core/version.h"

#include <fmt/format.h>

namespace pen {

namespace {

std::string compilerName() {
#if defined(_MSC_VER)
  return fmt::format("MSVC {}.{}", _MSC_VER / 100, _MSC_VER % 100);
#elif defined(__clang__)
  return fmt::format("Clang {}.{}", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
  return fmt::format("GCC {}.{}", __GNUC__, __GNUC_MINOR__);
#else
  return "unknown compiler";
#endif
}

constexpr std::string_view kPlatform =
#if defined(_WIN32)
    "Windows"
#elif defined(__linux__)
    "Linux"
#else
    "unknown OS"
#endif
#if defined(_M_X64) || defined(__x86_64__)
    " x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    " arm64";
#else
    "";
#endif

constexpr std::string_view kBuildType =
#if defined(NDEBUG)
    "Release";
#else
    "Debug";
#endif

}  // namespace

std::string versionString() {
  return fmt::format("Penumbra {} (git {}, {}, {}, {})", appVersion(), gitHash(), kBuildType, compilerName(),
                     kPlatform);
}

}  // namespace pen
