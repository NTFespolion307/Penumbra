#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "apps/penumbra/cli/cli.h"
#include "core/settings/schema.h"

using namespace pen;
using namespace pen::cli;

namespace {

const std::string kScene = std::string(PEN_TEST_DATA_DIR) + "/empty.gltf";

ParseResult run(std::vector<std::string> args) {
  ParseOptions o;
  o.load_user_config = false;
  return parse(args, o);
}

// Returns a copy: callers pass temporaries.
Invocation ok(const ParseResult& r) {
  INFO(r.err);
  REQUIRE_FALSE(r.exit_now);
  REQUIRE(r.invocation);
  return *r.invocation;
}

int errorCode(const ParseResult& r) {
  REQUIRE(r.exit_now);
  return r.exit_code;
}

}  // namespace

TEST_CASE("no arguments opens the UI", "[cli]") {
  const auto r = run({});
  CHECK(ok(r).mode == Mode::Ui);
}

TEST_CASE("headless controller", "[cli]") {
  const auto r = run({"--terminal", kScene, "--res", "4k", "--spp", "1024", "-o", "out.exr", "--max-bounces", "32",
                      "--no-rr"});
  const Invocation inv = ok(r);
  CHECK(inv.mode == Mode::Terminal);
  CHECK(inv.settings.res.width == 3840);
  CHECK(inv.settings.spp == 1024);
  CHECK(inv.settings.max_bounces == 32);
  CHECK(inv.settings.no_rr);
  CHECK(inv.settings.format == ImageFormat::Exr);
  CHECK(inv.settings.scene == kScene);
}

TEST_CASE("scene by --scene equals positional scene", "[cli]") {
  CHECK(ok(run({"--terminal", "--scene", kScene})).settings.scene == kScene);
}

TEST_CASE("terminal mode needs a scene", "[cli]") {
  const auto r = run({"--terminal"});
  CHECK(errorCode(r) == 1);
  CHECK(r.err.find("needs a scene") != std::string::npos);
}

TEST_CASE("headless host controller", "[cli]") {
  const Invocation inv = ok(run({"--terminal", "--host", kScene, "--port", "7421", "--code", "K7QD-M2XR-9FHT-4WNB"}));
  CHECK(inv.host);
  CHECK(inv.settings.port == 7421);
  CHECK(inv.code == "K7QD-M2XR-9FHT-4WNB");
  CHECK(errorCode(run({"--host", kScene})) == 1);  // needs --terminal
}

TEST_CASE("worker", "[cli]") {
  const Invocation inv =
      ok(run({"--worker", "--join", "192.168.1.20", "--code", "K7QD-M2XR-9FHT-4WNB", "--stay", "--devices", "0, cpu"}));
  CHECK(inv.mode == Mode::Worker);
  CHECK(inv.join == "192.168.1.20");
  CHECK(inv.settings.stay);
  CHECK(inv.settings.devices == "0,cpu");

  CHECK(errorCode(run({"--worker", "--code", "K7QD-M2XR-9FHT-4WNB"})) == 1);                          // no --join
  CHECK(errorCode(run({"--worker", "--join", "a", "--code", "b", "--host"})) == 1);                     // both roles
  CHECK(errorCode(run({"--worker", "--join", "a", "--code", "b", kScene})) == 1);                      // scene
  CHECK(errorCode(run({"--join", "192.168.1.20"})) == 1);                                               // no --worker
}

TEST_CASE("resume and merge", "[cli]") {
  CHECK(ok(run({"--resume", "render.session.json"})).mode == Mode::Resume);
  CHECK(errorCode(run({"--resume", "render.session.json", kScene})) == 1);

  const Invocation m = ok(run({"merge", "a.penfilm", "b.penfilm", "-o", "out.exr"}));
  CHECK(m.mode == Mode::Merge);
  CHECK(m.merge_inputs.size() == 2);
  CHECK(m.merge_output == "out.exr");
  CHECK(errorCode(run({"merge", "a.penfilm"})) == 1);  // -o required
}

TEST_CASE("info modes skip settings resolution", "[cli]") {
  CHECK(ok(run({"--version"})).mode == Mode::Version);
  CHECK(ok(run({"--list-devices"})).mode == Mode::ListDevices);
  CHECK(ok(run({"--diagnose", "--config", "missing.json"})).mode == Mode::Diagnose);
}

TEST_CASE("bad values exit with code 1 and name the flag", "[cli]") {
  const auto r = run({"--terminal", kScene, "--spp", "5000000000"});
  CHECK(errorCode(r) == 1);
  CHECK(r.err.find("--spp") != std::string::npos);
  CHECK(errorCode(run({"--terminal", kScene, "--res", "5k"})) == 1);
  CHECK(errorCode(run({"--terminal", kScene, "--bogus"})) == 1);
  CHECK(errorCode(run({"--terminal", kScene, "--res", "20k", "-o", "x.webp"})) == 1);
}

TEST_CASE("help exits 0 and lists every group", "[cli]") {
  const auto r = run({"--help"});
  CHECK(errorCode(r) == 0);
  for (const char* g : {"General", "Render", "Devices", "Output", "Film", "Controller", "Worker", "Info"})
    CHECK(r.out.find(g) != std::string::npos);
}

TEST_CASE("help and markdown cover every setting", "[cli]") {
  const std::string help = run({"--help"}).out;
  const std::string md = helpMarkdown();
  for (const auto& spec : settingSchema()) {
    INFO(std::string(spec.key));
    CHECK(help.find(std::string(longFlag(spec))) != std::string::npos);
    CHECK(md.find("`" + std::string(longFlag(spec)) + "`") != std::string::npos);
    CHECK(md.find("`" + std::string(spec.key) + "`") != std::string::npos);
  }
  for (const char* flag : {"--terminal", "--worker", "--join", "--code", "--host", "--resume", "--config", "--version",
                           "--list-devices", "--diagnose", "--benchmark"})
    CHECK(md.find(std::string("`") + flag + "`") != std::string::npos);
  const auto mdRun = run({"--help-markdown"});
  CHECK(errorCode(mdRun) == 0);
  CHECK(mdRun.out == md);
}
