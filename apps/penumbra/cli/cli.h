#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/settings/render_settings.h"

namespace pen::cli {

enum class Mode { Ui, Terminal, Worker, Resume, Merge, ListDevices, Version, Diagnose, Benchmark };
std::string_view toString(Mode m);

// What the user asked for: the mode, its mode-only arguments, and the fully resolved settings.
struct Invocation {
  Mode mode = Mode::Ui;
  bool terminal = false;
  bool host = false;
  std::string join;  // worker: controller address[:port]
  std::string code;  // join code (worker: required; controller: fixed code)
  std::filesystem::path resume;
  std::vector<std::filesystem::path> merge_inputs;
  std::filesystem::path merge_output;
  std::filesystem::path benchmark_output;  // --benchmark: the JSON report (-o)

  RenderSettings settings;  // resolved and finalized; not resolved for info modes
  nlohmann::json cli_settings = nlohmann::json::object();  // settings given explicitly on the command line
  std::filesystem::path config_file;
  std::vector<std::string> warnings;  // to log once logging is up
};

struct ParseOptions {
  bool load_user_config = true;
};

struct ParseResult {
  int exit_code = 0;
  bool exit_now = false;  // --help, --help-markdown or an error: print out/err and exit with exit_code
  std::string out;        // for stdout
  std::string err;        // for stderr
  std::optional<Invocation> invocation;
};

// args excludes the program name.
ParseResult parse(std::span<const std::string> args, const ParseOptions& options = {});

// The CLI reference table for README.md, generated from the same option list as --help.
std::string helpMarkdown();

// One line describing the settings that matter for a render, for the log.
std::string describe(const RenderSettings& s);

}  // namespace pen::cli
