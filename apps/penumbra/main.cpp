// penumbra: one binary for every role. No args = UI controller; --terminal = headless controller;
// --worker = headless worker. See docs/GUIDE.md "Application modes and CLI".

#include <cstdio>
#include <fmt/format.h>
#include <string>
#include <vector>

#include "apps/penumbra/cli/cli.h"
#include "apps/penumbra/modes/info.h"
#include "apps/penumbra/platform.h"
#include "apps/penumbra/progress.h"
#if defined(PEN_HAS_UI)
#include "apps/penumbra/ui/ui.h"
#endif
#include "controller/solo_render.h"
#include "worker/client/worker_client.h"
#include "core/exit_codes.h"
#include "core/log/log.h"
#include "core/system/paths.h"
#include "core/version.h"

namespace {

using pen::cli::Invocation;
using pen::cli::Mode;

void write(FILE* f, const std::string& s) { std::fwrite(s.data(), 1, s.size(), f); }

int notYet(std::string_view what, std::string_view milestone) {
  spdlog::error("{} is built in milestone {}; this build only validates its arguments.", what, milestone);
  return pen::toInt(pen::ExitCode::BadArguments);
}

#if !defined(PEN_HAS_UI)
int runUiPlaceholder(bool fromExplorer) {
  const std::string text = fmt::format(
      "{}\n\nThe Penumbra UI is built in milestone M10.\nFor now, run Penumbra from a terminal, for example:\n\n"
      "  penumbra --help\n  penumbra --diagnose",
      pen::versionString());
  spdlog::warn("the UI is built in milestone M10; run 'penumbra --help' for terminal modes");
  if (fromExplorer) pen::app::showMessage("Penumbra", text, false);
  return pen::toInt(pen::ExitCode::BadArguments);
}
#endif

int dispatch(const Invocation& inv, bool fromExplorer) {
  pen::LocalRenderOptions render;
  render.progress = pen::app::makeProgressPrinter(inv.settings.progress);
  switch (inv.mode) {
#if defined(PEN_HAS_UI)
    case Mode::Ui: (void)fromExplorer; return pen::ui::run(inv.settings);
#else
    case Mode::Ui: return runUiPlaceholder(fromExplorer);
#endif
    case Mode::Terminal:
      spdlog::info("render: {}", pen::cli::describe(inv.settings));
      render.interrupt = &pen::app::installInterruptHandler();
      render.host = inv.host;
      render.code = inv.code;
      return pen::runLocalRender(inv.settings, render);
    case Mode::Worker: {
      pen::WorkerOptions worker;
      worker.settings = inv.settings;
      worker.join = inv.join;
      worker.code = inv.code;
      worker.interrupt = &pen::app::installInterruptHandler();
      return pen::runWorker(worker);
    }
    case Mode::Resume:
      if (!inv.terminal) spdlog::info("the UI is built in milestone M10; resuming in the terminal");
      render.interrupt = &pen::app::installInterruptHandler();
      return pen::resumeLocalRender(inv.resume, inv.cli_settings, render);
    case Mode::Merge: return pen::mergeFilmsToImage(inv.merge_inputs, inv.settings);
    case Mode::Benchmark: return notYet("The benchmark", "M9");
    default: return pen::toInt(pen::ExitCode::BadArguments);
  }
}

}  // namespace

int main(int argc, char** argv) {
  pen::app::ConsoleSetup console;
  const bool aloneInConsole = pen::app::consoleOwnedAlone();
  const std::vector<std::string> args(argv + 1, argv + argc);

  const pen::cli::ParseResult parsed = pen::cli::parse(args);
  if (parsed.exit_now) {
    if (aloneInConsole && parsed.exit_code != 0) {
      // Started from Explorer (e.g. a file dropped on the exe): a console would flash and vanish.
      pen::app::detachConsole();
      pen::app::showMessage("Penumbra", parsed.err, true);
    } else {
      write(stdout, parsed.out);
      write(stderr, parsed.err);
    }
    return parsed.exit_code;
  }

  const Invocation& inv = *parsed.invocation;
  switch (inv.mode) {
    case Mode::Version: return pen::app::printVersion();
    case Mode::ListDevices: return pen::app::listDevices();
    case Mode::Diagnose: return pen::app::diagnose();
    default: break;
  }

  const bool fromExplorer = inv.mode == Mode::Ui && aloneInConsole;
  if (fromExplorer) pen::app::detachConsole();  // UI launched by double-click: no console window

  pen::LogOptions logOptions;
  logOptions.role = inv.mode == Mode::Worker ? pen::Role::Worker : pen::Role::Controller;
  logOptions.level = inv.settings.log_level;
  logOptions.file = inv.settings.log_file;
  logOptions.console = !fromExplorer;
  const auto logFile = pen::initLogging(logOptions);

  spdlog::info("{}", pen::versionString());
  spdlog::info("mode {}, log file {}", pen::cli::toString(inv.mode),
               logFile.empty() ? std::string("(none)") : pen::pathToUtf8(logFile));
  if (!inv.config_file.empty()) spdlog::info("config file {}", pen::pathToUtf8(inv.config_file));
  for (const auto& w : inv.warnings) spdlog::warn("{}", w);

  const int code = dispatch(inv, fromExplorer);
  pen::shutdownLogging();
  return code;
}
