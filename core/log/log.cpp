#include "core/log/log.h"

#include <fmt/format.h>
#include <memory>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

#include "core/system/paths.h"
#include "core/system/sysinfo.h"

namespace pen {

namespace {
spdlog::level::level_enum toSpd(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warn: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    case LogLevel::Off: return spdlog::level::off;
  }
  return spdlog::level::info;
}
}  // namespace

std::filesystem::path initLogging(const LogOptions& options) {
  std::vector<spdlog::sink_ptr> sinks;
  if (options.console) {
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    sinks.push_back(console);
  }

  std::filesystem::path file = options.file;
  if (file.empty()) file = paths::logDir() / fmt::format("penumbra-{}.log", toString(options.role));
  std::string fileError;
  try {
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    auto rotating = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(pathToUtf8(file), 10 * 1024 * 1024, 5);
    rotating->set_pattern(fmt::format("[%Y-%m-%d %H:%M:%S.%e] [%l] [{}@{}] %v", toString(options.role), hostName()));
    sinks.push_back(rotating);
  } catch (const std::exception& e) {
    fileError = e.what();
    file.clear();
  }

  auto logger = std::make_shared<spdlog::logger>("penumbra", sinks.begin(), sinks.end());
  logger->set_level(toSpd(options.level));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(logger);
  spdlog::flush_every(std::chrono::seconds(2));

  if (!fileError.empty()) spdlog::warn("cannot open log file, logging to the console only: {}", fileError);
  return file;
}

void shutdownLogging() { spdlog::shutdown(); }

}  // namespace pen
