#pragma once

namespace pen::app {

// Print-and-exit modes. Each writes to stdout and returns the process exit code.
int printVersion();
int listDevices();
int diagnose();

}  // namespace pen::app
