#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/config/batch.h"
#include "core/settings/schema.h"
#include "core/system/paths.h"

using namespace pen;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

// A temporary folder removed at the end of the test.
struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("penumbra-test-" + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path write(const std::string& name, const std::string& content) const {
    std::ofstream(path / name, std::ios::binary) << content;
    return path / name;
  }
  std::string str(const std::string& name) const { return pathToUtf8(path / name); }
};

// Escapes a path for a JSON string.
std::string js(const std::string& s) {
  std::string out;
  for (const char c : s) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
  return out;
}

SettingsSources noUserConfig() {
  SettingsSources src;
  src.load_user_config = false;
  return src;
}

// The message of the SettingsError loadBatch throws, or "" when it does not.
std::string batchError(const fs::path& file, const SettingsSources& base, bool hosting = false) {
  try {
    loadBatch(file, base, hosting);
  } catch (const SettingsError& e) {
    return e.what();
  }
  return "";
}

}  // namespace

TEST_CASE("batch jobs: job > command line > .pen.json, names and the scene default", "[batch]") {
  TempDir dir;
  dir.write("a.glb", "placeholder");
  dir.write("b.glb", "placeholder");
  dir.write("shot.pen.json", R"({ "scene": "b.glb", "render": { "spp": 256, "tonemap": "filmic", "res": "4k" } })");
  const fs::path file = dir.write("jobs.json", R"({ "jobs": [
    { "name": "wide", "output": ")" + js(dir.str("wide.png")) + R"(", "res": "1080p" },
    { "scene": ")" + js(dir.str("shot.pen.json")) + R"(", "output": ")" + js(dir.str("shot.exr")) + R"(", "spp": 512 }
  ] })");
  SettingsSources base = noUserConfig();
  base.cli = {{"scene", dir.str("a.glb")}, {"spp", "64"}, {"tonemap", "aces"}};
  const std::vector<BatchJob> jobs = loadBatch(file, base, false);
  REQUIRE(jobs.size() == 2);

  CHECK(jobs[0].name == "wide");
  CHECK(jobs[0].settings.scene == dir.path / "a.glb");  // the command line's scene is the default
  CHECK(jobs[0].settings.res.preset == "1080p");
  CHECK(jobs[0].settings.spp == 64);  // command line

  CHECK(jobs[1].name == "job 2");
  CHECK(jobs[1].settings.scene == dir.path / "b.glb");  // the job's .pen.json replaces the command line's scene
  CHECK(jobs[1].settings.spp == 512);                    // job beats command line and .pen.json
  CHECK(jobs[1].settings.tonemap == Tonemap::Aces);      // command line beats .pen.json
  CHECK(jobs[1].settings.res.preset == "4k");            // .pen.json beats the default
  CHECK(jobs[0].fingerprint != jobs[1].fingerprint);

  // A bare list works too, and gives the same jobs.
  const fs::path bare = dir.write("bare.json", R"([ { "output": ")" + js(dir.str("wide.png")) + R"(", "res": "1080p" } ])");
  const std::vector<BatchJob> one = loadBatch(bare, base, false);
  REQUIRE(one.size() == 1);
  CHECK(one[0].fingerprint == jobs[0].fingerprint);
}

TEST_CASE("batch files are checked before anything renders", "[batch]") {
  TempDir dir;
  dir.write("a.glb", "placeholder");
  const std::string scene = js(dir.str("a.glb"));
  const SettingsSources base = noUserConfig();
  const auto bad = [&](const std::string& content) { return batchError(dir.write("jobs.json", content), base); };

  CHECK_THAT(bad("not json"), ContainsSubstring("not valid JSON"));
  CHECK_THAT(bad(R"({ "jobs": [] })"), ContainsSubstring("one or more"));
  CHECK_THAT(bad(R"({ "job": [] })"), ContainsSubstring("unknown key 'job'"));
  CHECK_THAT(bad(R"([ 3 ])"), ContainsSubstring("job 1: must be an object"));
  CHECK_THAT(bad(R"([ { "scene": ")" + scene + R"(", "sp": 64 } ])"), ContainsSubstring("job 1: unknown setting 'sp'"));
  CHECK_THAT(bad(R"([ { "output": "x.png" } ])"), ContainsSubstring("no scene"));
  CHECK_THAT(bad(R"([ { "scene": ")" + scene + R"(", "spp": "lots" } ])"), ContainsSubstring("jobs.json, job 1: spp"));
  CHECK_THAT(bad(R"([ { "scene": "missing.glb" } ])"), ContainsSubstring("scene file not found"));
  // Two jobs writing the same output, written differently, or the same film (same stem, other format).
  CHECK_THAT(bad(R"([ { "scene": ")" + scene + R"(", "output": "out/x.png" },
                     { "scene": ")" + scene + R"(", "output": "out/../out/x.png", "spp": 8 } ])"),
             ContainsSubstring("job 1 and job 2 would both write the output"));
  CHECK_THAT(bad(R"([ { "scene": ")" + scene + R"(", "output": "x.png" }, { "scene": ")" + scene + R"(", "output": "x.exr" } ])"),
             ContainsSubstring("would both write the film"));

  // Hosting: the workers' session settings must agree; without hosting they may differ.
  const fs::path ports = dir.write("ports.json", R"([ { "scene": ")" + scene + R"(", "output": "a.png" },
                                                       { "scene": ")" + scene + R"(", "output": "b.png", "port": 7421 } ])");
  CHECK(batchError(ports, base, false).empty());
  CHECK_THAT(batchError(ports, base, true), ContainsSubstring("job 2 has a different 'port'"));
}

TEST_CASE("batch state: finished jobs by fingerprint, next to the batch file", "[batch]") {
  TempDir dir;
  const fs::path file = dir.path / "nightly.json";
  const fs::path state = batchStatePath(file);
  CHECK(state == dir.path / "nightly.batch-state.json");
  CHECK(loadBatchState(state).empty());  // missing
  saveBatchState(state, {1, 0xfedcba9876543210ull});
  CHECK(loadBatchState(state) == std::set<uint64_t>{1, 0xfedcba9876543210ull});
  dir.write("nightly.batch-state.json", "{ damaged");
  CHECK(loadBatchState(state).empty());
}
