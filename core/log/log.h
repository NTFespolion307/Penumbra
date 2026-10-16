#pragma once

#include <filesystem>
#include <spdlog/spdlog.h>

#include "core/settings/render_settings.h"

namespace pen {

enum class Role { Controller, Worker };

constexpr const char* toString(Role r) { return r == Role::Controller ? "controller" : "worker"; }

struct LogOptions {
  Role role = Role::Controller;
  LogLevel level = LogLevel::Info;
  std::filesystem::path file;  // empty: <logDir>/penumbra-<role>.log
  bool console = true;         // log to stderr (stdout stays free for --progress json)
};

// Sets up the default spdlog logger: stderr + rotating file (10 MB x 5). Each file line carries timestamp,
// level, role and host name. Returns the log file in use, or an empty path if it could not be opened (a
// warning is logged and logging continues on the console only).
std::filesystem::path initLogging(const LogOptions& options);
void shutdownLogging();

}  // namespace pen
