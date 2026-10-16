#include "apps/penumbra/platform.h"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace pen::app {

#if defined(_WIN32)

namespace {
std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}
}  // namespace

ConsoleSetup::ConsoleSetup() {
  old_output_cp_ = GetConsoleOutputCP();
  if (old_output_cp_ != 0) SetConsoleOutputCP(CP_UTF8);
}

ConsoleSetup::~ConsoleSetup() {
  if (old_output_cp_ != 0) SetConsoleOutputCP(old_output_cp_);
}

bool consoleOwnedAlone() {
  DWORD ids[2];
  return GetConsoleProcessList(ids, 2) == 1;
}

void detachConsole() { FreeConsole(); }

void showMessage(std::string_view title, std::string_view text, bool isError) {
  MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(),
              MB_OK | MB_SETFOREGROUND | (isError ? MB_ICONERROR : MB_ICONINFORMATION));
}

#else

ConsoleSetup::ConsoleSetup() = default;
ConsoleSetup::~ConsoleSetup() = default;
bool consoleOwnedAlone() { return false; }
void detachConsole() {}
void showMessage(std::string_view title, std::string_view text, bool) {
  std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(title.size()), title.data(), static_cast<int>(text.size()),
               text.data());
}

#endif

}  // namespace pen::app
