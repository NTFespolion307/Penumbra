#pragma once

#include <atomic>
#include <string_view>

namespace pen::app {

// Windows: switches the console to UTF-8 for the lifetime of the object and restores it afterwards.
class ConsoleSetup {
 public:
  ConsoleSetup();
  ~ConsoleSetup();
  ConsoleSetup(const ConsoleSetup&) = delete;
  ConsoleSetup& operator=(const ConsoleSetup&) = delete;

 private:
  unsigned old_output_cp_ = 0;
};

// True when this process is the only one attached to its console, i.e. Windows created a console just for
// us because we were started from Explorer (double-click, shortcut). Always false on Linux.
bool consoleOwnedAlone();

// Closes the console window we were given (Windows; no-op on Linux).
void detachConsole();

// Shows a message box on Windows; prints to stderr on Linux.
void showMessage(std::string_view title, std::string_view text, bool isError);

// Ctrl+C (and SIGTERM / console close) sets the returned flag so the render can checkpoint and exit with
// code 5; a second Ctrl+C exits immediately.
const std::atomic<bool>& installInterruptHandler();

}  // namespace pen::app
