#include "apps/penumbra/platform.h"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace pen::app {

namespace {
std::atomic<bool> g_interrupted{false};
constexpr int kInterruptedExit = 5;
}  // namespace

#if defined(_WIN32)

namespace {
BOOL WINAPI consoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    if (g_interrupted.exchange(true)) ExitProcess(kInterruptedExit);  // second Ctrl+C: leave now
    return TRUE;
  }
  return FALSE;
}
}  // namespace

const std::atomic<bool>& installInterruptHandler() {
  SetConsoleCtrlHandler(consoleHandler, TRUE);
  return g_interrupted;
}

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

namespace {
void signalHandler(int) {
  if (g_interrupted.exchange(true)) _exit(kInterruptedExit);  // second signal: leave now
}
}  // namespace

const std::atomic<bool>& installInterruptHandler() {
  struct sigaction sa {};
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  return g_interrupted;
}

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
