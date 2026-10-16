#pragma once

namespace pen {

// Process exit codes (docs/GUIDE.md "Application modes and CLI"). Scripts depend on these; never renumber.
enum class ExitCode : int {
  Success = 0,
  BadArguments = 1,      // also: mode not implemented yet in this milestone
  SceneLoadError = 2,
  NoUsableDevice = 3,
  OutOfDisk = 4,
  Interrupted = 5,       // checkpoint written
  RejectedByController = 6,
  ConnectionLost = 7,    // worker without --stay
};

constexpr int toInt(ExitCode c) { return static_cast<int>(c); }

}  // namespace pen
