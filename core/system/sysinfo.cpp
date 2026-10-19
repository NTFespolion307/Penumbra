#include "core/system/sysinfo.h"

#include <fmt/format.h>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

namespace pen {

namespace {

#if defined(_WIN32)

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

std::string windowsVersion() {
  // GetVersionEx lies to unmanifested callers; RtlGetVersion does not.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&vi);
  }
  const char* name = vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
  const std::string display =
      narrow(regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion"));
  return display.empty() ? fmt::format("{} (build {})", name, vi.dwBuildNumber)
                         : fmt::format("{} ({}, build {})", name, display, vi.dwBuildNumber);
}

#else

std::string readKey(const char* file, const std::string& key, char sep) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    const size_t pos = line.find(sep);
    if (pos == std::string::npos) continue;
    std::string v = line.substr(pos + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '"')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '"' || v.back() == '\r')) v.pop_back();
    return v;
  }
  return {};
}

uint64_t meminfoBytes(const std::string& key) {
  const std::string v = readKey("/proc/meminfo", key + ":", ':');
  return v.empty() ? 0 : std::stoull(v) * 1024;  // "16303208 kB"
}

#endif

}  // namespace

std::string hostName() {
#if defined(_WIN32)
  wchar_t buf[256];
  DWORD size = 256;
  if (GetComputerNameExW(ComputerNameDnsHostname, buf, &size)) return narrow(std::wstring(buf, size));
  return "unknown-host";
#else
  char buf[256] = {};
  if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
  return "unknown-host";
#endif
}

SystemInfo querySystem() {
  SystemInfo info;
  info.host = hostName();
  info.logical_cores = std::thread::hardware_concurrency();
#if defined(_WIN32)
  info.os = windowsVersion();
  info.cpu = narrow(regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                              L"ProcessorNameString"));
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    info.ram_total = ms.ullTotalPhys;
    info.ram_available = ms.ullAvailPhys;
  }
#else
  std::string pretty = readKey("/etc/os-release", "PRETTY_NAME=", '=');
  if (pretty.empty()) pretty = "Linux";
  utsname u{};
  info.os = uname(&u) == 0 ? fmt::format("{} (kernel {})", pretty, u.release) : pretty;
  info.cpu = readKey("/proc/cpuinfo", "model name", ':');
  info.ram_total = meminfoBytes("MemTotal");
  info.ram_available = meminfoBytes("MemAvailable");
#endif
  while (!info.cpu.empty() && info.cpu.back() == ' ') info.cpu.pop_back();
  if (info.cpu.empty()) info.cpu = "unknown CPU";
  return info;
}

double CpuUsage::sample() {
  uint64_t idle = 0, total = 0;
#if defined(_WIN32)
  FILETIME fi, fk, fu;
  if (!GetSystemTimes(&fi, &fk, &fu)) return 0;
  const auto u64 = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  idle = u64(fi);
  total = u64(fk) + u64(fu);  // kernel time includes idle time
#else
  std::ifstream in("/proc/stat");
  std::string cpu;
  uint64_t v[8] = {};
  in >> cpu >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7];
  if (cpu != "cpu") return 0;
  idle = v[3] + v[4];
  for (uint64_t x : v) total += x;
#endif
  const uint64_t dIdle = idle - idle_, dTotal = total - total_;
  const bool first = total_ == 0;
  idle_ = idle;
  total_ = total;
  if (first || dTotal == 0) return 0;
  return 100.0 * double(dTotal - dIdle) / double(dTotal);
}

}  // namespace pen
