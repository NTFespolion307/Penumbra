#include "apps/penumbra/cli/cli.h"

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

#include "core/config/config.h"
#include "core/exit_codes.h"
#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/system/paths.h"
#include "core/version.h"

namespace pen::cli {

std::string_view toString(Mode m) {
  switch (m) {
    case Mode::Ui: return "ui";
    case Mode::Terminal: return "terminal";
    case Mode::Worker: return "worker";
    case Mode::Resume: return "resume";
    case Mode::Merge: return "merge";
    case Mode::ListDevices: return "list-devices";
    case Mode::Version: return "version";
    case Mode::Diagnose: return "diagnose";
    case Mode::Benchmark: return "benchmark";
  }
  return "?";
}

namespace {

// One row of the CLI: either a persistent setting (spec != nullptr) or a mode-only flag.
struct OptionRow {
  std::string_view names;
  std::string_view group;
  OptionKind kind;
  std::string_view values;
  std::string_view default_text;
  std::string_view example;
  std::string_view help;
  const SettingSpec* spec = nullptr;
};

constexpr OptionKind kFlag = OptionKind::Flag;
constexpr OptionKind kValue = OptionKind::Value;

// Mode-only flags: never stored in the config file or .pen.json.
const std::vector<OptionRow> kModeRowsBefore = {
    {"--terminal", "General", kFlag, "", "off", "--terminal scene.glb -o out.png",
     "Headless controller: no window, no display server needed"},
    {"--config", "General", kValue, "path", "per-OS user config", "--config defaults.json", "Defaults file to use"},
    {"--resume", "Film", kValue, "session file", "", "--resume render.session.json",
     "Continue an interrupted render (add --terminal for headless)"},
    {"--host", "Controller", kFlag, "", "off", "--terminal --host scene.glb",
     "Accept workers (headless controller; needs --terminal)"},
    {"--worker", "Worker", kFlag, "", "off", "--worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB",
     "Run as a headless worker"},
    {"--join", "Worker", kValue, "address[:port]", "", "--join 203.0.113.5:7420",
     "Controller to join (LAN IP, public IP, VPN IP or host name)"},
    {"--code", "Worker", kValue, "XXXX-XXXX-XXXX-XXXX", "env PENUMBRA_CODE", "--code K7QD-M2XR-9FHT-4WNB",
     "Join code. Workers need it; a controller uses it as a fixed code instead of the saved one"},
};
const std::vector<OptionRow> kInfoRows = {
    {"--version", "Info", kFlag, "", "", "--version", "Print version and git hash, then exit"},
    {"--list-devices", "Info", kFlag, "", "", "--list-devices", "List this machine's render devices, then exit"},
    {"--diagnose", "Info", kFlag, "", "", "--diagnose", "Print OS, devices, paths and free disk for bug reports"},
    {"--benchmark", "Info", kFlag, "", "", "--benchmark", "Run the benchmark suite (from M9)"},
};
constexpr std::array<std::string_view, 7> kGroups = {"General", "Render", "Devices", "Output",
                                                     "Film", "Controller", "Worker"};

std::vector<OptionRow> allRows() {
  std::vector<OptionRow> rows;
  for (std::string_view g : kGroups) {
    for (const auto& r : kModeRowsBefore)
      if (r.group == g) rows.push_back(r);
    for (const auto& s : settingSchema()) {
      if (s.group != g) continue;
      rows.push_back({s.names, s.group, s.kind, s.values, s.default_text, s.example, s.help, &s});
    }
  }
  rows.insert(rows.end(), kInfoRows.begin(), kInfoRows.end());
  return rows;
}

std::string optionDescription(const OptionRow& r) {
  std::string d(r.help);
  if (!r.values.empty()) d += fmt::format("\n  values: {}", r.values);
  if (!r.default_text.empty() && r.kind == kValue) d += fmt::format("\n  default: {}", r.default_text);
  return d;
}

constexpr std::string_view kFooter = R"(Modes:
  penumbra                                    UI as controller (from M10)
  penumbra --terminal scene.glb -o out.exr    headless controller, local devices only
  penumbra --terminal --host scene.glb        headless controller that accepts workers
  penumbra --worker --join ADDRESS --code CODE [--stay]   headless worker
  penumbra --resume render.session.json       resume an interrupted render
  penumbra merge a.penfilm b.penfilm -o out.exr   combine partial films

Exit codes: 0 success, 1 bad arguments, 2 scene load error, 3 no usable device, 4 out of disk,
5 interrupted (checkpoint written), 6 rejected by controller, 7 connection lost.
Settings precedence: command line > .pen.json "render" > user config file > defaults.)";

}  // namespace

ParseResult parse(std::span<const std::string> args, const ParseOptions& options) {
  ParseResult result;
  const auto fail = [&](const std::string& msg, int code = toInt(ExitCode::BadArguments)) {
    result.exit_now = true;
    result.exit_code = code;
    result.err = fmt::format("error: {}\nRun with --help for more information.\n", msg);
    return result;
  };

  CLI::App app{fmt::format("Penumbra {}: distributed multi-GPU path tracer", appVersion()), "penumbra"};
  app.footer(std::string(kFooter));
  app.get_formatter()->column_width(34);

  const std::vector<OptionRow> rows = allRows();
  std::map<std::string_view, CLI::Option*> byName;  // long flag -> option
  std::vector<std::pair<const SettingSpec*, CLI::Option*>> settingOptions;
  for (const auto& r : rows) {
    CLI::Option* opt = r.kind == kFlag ? app.add_flag(std::string(r.names)) : app.add_option(std::string(r.names));
    opt->description(optionDescription(r))->group(std::string(r.group));
    if (r.kind == kValue) opt->type_name("");
    if (r.spec) {
      settingOptions.emplace_back(r.spec, opt);
    } else {
      byName[r.names] = opt;
    }
  }
  byName["--code"]->envname("PENUMBRA_CODE");
  CLI::Option* helpMd = app.add_flag("--help-markdown")->group("");  // hidden: regenerates the README table

  std::vector<std::string> mergeInputs;
  std::string mergeOutput;
  CLI::App* merge = app.add_subcommand("merge", "Combine partial films (.penfilm) into one output image");
  merge->add_option("films", mergeInputs, "Partial films to combine")->required();
  merge->add_option("-o,--output", mergeOutput, "Output image")->required();

  std::vector<std::string> reversed(args.rbegin(), args.rend());  // CLI11 consumes from the back
  try {
    app.parse(reversed);
  } catch (const CLI::ParseError& e) {
    std::ostringstream out, err;
    const int code = app.exit(e, out, err);
    result.exit_now = true;
    result.exit_code = code == 0 ? 0 : toInt(ExitCode::BadArguments);
    result.out = out.str();
    result.err = err.str();
    return result;
  }

  if (helpMd->count()) {
    result.exit_now = true;
    result.out = helpMarkdown();
    return result;
  }

  Invocation inv;
  const auto given = [&](std::string_view name) { return byName.at(name)->count() > 0; };
  const auto value = [&](std::string_view name) { return byName.at(name)->as<std::string>(); };
  bool sceneGiven = false;
  for (const auto& [spec, opt] : settingOptions)
    if (spec->key == "scene" && opt->count()) sceneGiven = true;

  inv.terminal = given("--terminal");
  inv.host = given("--host");
  if (given("--join")) inv.join = value("--join");
  if (given("--code")) inv.code = value("--code");
  if (given("--resume")) inv.resume = pathFromUtf8(value("--resume"));

  SettingsSources sources;
  sources.load_user_config = options.load_user_config;
  if (given("--config")) sources.config_file = pathFromUtf8(value("--config"));
  for (const auto& [spec, opt] : settingOptions) {
    if (!opt->count()) continue;
    if (spec->kind == OptionKind::Flag) sources.cli[std::string(spec->key)] = true;
    else sources.cli[std::string(spec->key)] = opt->as<std::string>();
  }
  const auto resolveInto = [&](Invocation& target) -> bool {
    try {
      ResolvedSettings resolved = resolveSettings(sources);
      target.settings = std::move(resolved.settings);
      target.config_file = std::move(resolved.config_file);
      target.warnings = std::move(resolved.warnings);
      target.cli_settings = sources.cli;
      return true;
    } catch (const SettingsError& e) {
      fail(e.what(), e.exit_code);
      return false;
    }
  };

  if (merge->parsed()) {
    inv.mode = Mode::Merge;
    for (const auto& f : mergeInputs) inv.merge_inputs.push_back(pathFromUtf8(f));
    inv.merge_output = pathFromUtf8(mergeOutput);
    if (sceneGiven) return fail("merge takes films, not a scene");
    sources.cli["output"] = mergeOutput;  // output options (exposure, format...) come from the root flags
    if (!resolveInto(inv)) return result;
    result.invocation = std::move(inv);
    return result;
  }
  if (given("--version")) inv.mode = Mode::Version;
  else if (given("--list-devices")) inv.mode = Mode::ListDevices;
  else if (given("--diagnose")) inv.mode = Mode::Diagnose;
  if (inv.mode != Mode::Ui) {
    result.invocation = std::move(inv);
    return result;
  }

  const bool worker = given("--worker");
  if (worker) {
    if (inv.host) return fail("--worker and --host cannot be combined: a machine is either the controller or a worker");
    if (sceneGiven || !inv.resume.empty())
      return fail("a worker gets its scene from the controller; remove the scene and --resume");
    if (inv.join.empty()) return fail("--worker needs --join <address>[:port]");
    if (inv.code.empty()) return fail("--worker needs --code XXXX-XXXX-XXXX-XXXX (or the PENUMBRA_CODE variable)");
    inv.mode = Mode::Worker;
  } else {
    if (!inv.join.empty()) return fail("--join only works together with --worker");
    if (inv.host && !inv.terminal)
      return fail("--host is for headless controllers: add --terminal (in the UI, hosting is on the Settings page)");
    if (!inv.resume.empty() && sceneGiven)
      return fail("--resume continues the scene stored in the session file; remove the scene argument");
    if (inv.terminal && inv.resume.empty() && !sceneGiven)
      return fail("--terminal needs a scene, for example: penumbra --terminal scene.glb -o out.png");
    if (!inv.resume.empty()) inv.mode = Mode::Resume;
    else if (given("--benchmark")) inv.mode = Mode::Benchmark;
    else if (inv.terminal) inv.mode = Mode::Terminal;
  }

  if (inv.mode == Mode::Resume) sources.load_user_config = false;  // the session file holds the settings
  if (!resolveInto(inv)) return result;
  result.invocation = std::move(inv);
  return result;
}

std::string helpMarkdown() {
  const auto cell = [](std::string_view s) {
    std::string out(s);
    for (size_t pos = 0; (pos = out.find('|', pos)) != std::string::npos; pos += 2) out.replace(pos, 1, "\\|");
    return out;
  };
  const auto code = [&](std::string_view s) { return s.empty() ? std::string() : fmt::format("`{}`", cell(s)); };

  std::string md =
      "| Group | Flag | Values | Default | Example | Config / `.pen.json` key | Description |\n"
      "| --- | --- | --- | --- | --- | --- | --- |\n";
  for (const auto& r : allRows()) {
    std::string names;
    std::string_view n = r.names;
    while (!n.empty()) {
      const size_t comma = n.find(',');
      const std::string_view part = n.substr(0, comma);
      if (part.starts_with("-")) names += (names.empty() ? "" : ", ") + code(part);
      else names += (names.empty() ? "" : ", ") + fmt::format("`<{}>`", part);
      if (comma == std::string_view::npos) break;
      n.remove_prefix(comma + 1);
    }
    const std::string example =
        r.example.starts_with("penumbra") ? code(r.example) : code(fmt::format("penumbra {}", r.example));
    md += fmt::format("| {} | {} | {} | {} | {} | {} | {} |\n", r.group, names, cell(r.values), cell(r.default_text),
                      example, r.spec ? code(r.spec->key) : "CLI only", cell(r.help));
  }
  md += "| Subcommand | `merge <films...> -o <output>` |  |  | `penumbra merge a.penfilm b.penfilm -o out.exr` "
        "| CLI only | Combine partial films into one output image |\n";
  return md;
}

std::string describe(const RenderSettings& s) {
  std::string scene = s.scene.empty() ? "(none)" : pathToUtf8(s.scene);
  if (!s.scene_description.empty()) scene += fmt::format(" (from {})", pathToUtf8(s.scene_description));
  const std::string res = s.res.preset.empty() ? "custom" : s.res.preset;
  return fmt::format(
      "scene {}, {}x{} ({}{}), {} spp, {} bounces, integrator {}, seed {}, backend {}, devices {}, output {} ({} "
      "{}-bit, tonemap {}, denoise {})",
      scene, s.outputWidth(), s.outputHeight(), res, s.scale != 1.0 ? fmt::format(" at scale {}", s.scale) : "",
      s.spp == 0 ? std::string("until time limit") : std::to_string(s.spp), s.max_bounces, toString(s.integrator),
      s.seed, toString(s.backend), s.devices, pathToUtf8(s.output), s.format ? toString(*s.format) : "?",
      s.bit_depth, toString(s.tonemap), toString(s.denoise));
}

}  // namespace pen::cli
