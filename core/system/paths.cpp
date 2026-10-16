#include "core/system/paths.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace pen {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

namespace paths {

namespace {

#if defined(_WIN32)
std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) result = raw;
  CoTaskMemFree(raw);
  return result;
}
std::filesystem::path roaming() { return knownFolder(FOLDERID_RoamingAppData) / "Penumbra"; }
std::filesystem::path local() { return knownFolder(FOLDERID_LocalAppData) / "Penumbra"; }
#else
std::filesystem::path home() {
  if (const char* h = std::getenv("HOME"); h && *h) return h;
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  return "/tmp";
}
// XDG base directory with fallback, e.g. xdg("XDG_CONFIG_HOME", ".config").
std::filesystem::path xdg(const char* var, const char* fallback) {
  if (const char* v = std::getenv(var); v && *v == '/') return std::filesystem::path(v) / "penumbra";
  return home() / fallback / "penumbra";
}
#endif

}  // namespace

std::filesystem::path userConfigFile() {
#if defined(_WIN32)
  return roaming() / "config.json";
#else
  return xdg("XDG_CONFIG_HOME", ".config") / "config.json";
#endif
}

std::filesystem::path logDir() {
#if defined(_WIN32)
  return local() / "logs";
#else
  return xdg("XDG_STATE_HOME", ".local/state") / "logs";
#endif
}

std::filesystem::path sceneCacheDir() {
#if defined(_WIN32)
  return local() / "cache" / "scenes";
#else
  return xdg("XDG_CACHE_HOME", ".cache") / "scenes";
#endif
}

std::filesystem::path userDataDir() {
#if defined(_WIN32)
  return local();
#else
  return xdg("XDG_DATA_HOME", ".local/share");
#endif
}

}  // namespace paths
}  // namespace pen
