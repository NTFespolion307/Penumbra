#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/config/config.h"
#include "core/settings/schema.h"

using namespace pen;
namespace fs = std::filesystem;

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
};

const fs::path kData = PEN_TEST_DATA_DIR;

}  // namespace

TEST_CASE("defaults without any layer", "[config]") {
  SettingsSources src;
  src.load_user_config = false;
  const ResolvedSettings r = resolveSettings(src);
  CHECK(r.settings.res.preset == "1080p");
  CHECK(r.settings.spp == 128);
  CHECK(r.settings.max_bounces == 12);
  CHECK(r.settings.port == 7420);
  CHECK(r.settings.tonemap == Tonemap::Agx);
  CHECK(r.settings.format == ImageFormat::Png);
  CHECK(r.config_file.empty());
  CHECK(r.warnings.empty());
}

TEST_CASE("precedence: command line > .pen.json > config file > defaults", "[config]") {
  TempDir dir;
  dir.write("scene.glb", "placeholder");
  const fs::path cfg = dir.write("config.json", R"({
    // comments are allowed in config files
    "spp": 64, "max_bounces": 4, "tonemap": "aces", "worker_name": "laptop", "exposure": 1.0
  })");
  const fs::path pen = dir.write("shot.pen.json", R"({
    "scene": "scene.glb",
    "camera": "Cam.002",
    "environment": { "hdri": "sky.exr", "strength": 0.5, "rotation_deg": 45 },
    "render": { "spp": 256, "tonemap": "filmic", "res": "4k" }
  })");

  SettingsSources src;
  src.config_file = cfg;
  src.cli = {{"scene", pen.string()}, {"spp", "1024"}, {"camera", "Cam.003"}};
  const ResolvedSettings r = resolveSettings(src);
  const RenderSettings& s = r.settings;

  CHECK(s.spp == 1024);                  // CLI beats .pen.json and config
  CHECK(s.camera == "Cam.003");          // CLI beats .pen.json
  CHECK(s.tonemap == Tonemap::Filmic);   // .pen.json beats config
  CHECK(s.res.preset == "4k");           // .pen.json beats default
  CHECK(s.max_bounces == 4);             // config beats default
  CHECK(s.worker_name == "laptop");      // config only
  CHECK(s.exposure == 1.0);
  CHECK(s.integrator == Integrator::Path);  // default
  CHECK(s.scene == dir.path / "scene.glb");
  CHECK(s.scene_description == pen);
  CHECK(s.environment.hdri == dir.path / "sky.exr");
  CHECK(s.environment.strength == 0.5);
  CHECK(s.environment.rotation_deg == 45);
  CHECK(r.config_file == cfg);
}

TEST_CASE("join code and unknown keys in files are ignored with a warning", "[config]") {
  TempDir dir;
  const fs::path cfg = dir.write("config.json", R"({ "code": "K7QD-M2XR-9FHT-4WNB", "spp": 32, "colour": "red" })");
  SettingsSources src;
  src.config_file = cfg;
  const ResolvedSettings r = resolveSettings(src);
  CHECK(r.settings.spp == 32);
  REQUIRE(r.warnings.size() == 2);
  CHECK(r.warnings[0].find("join code") != std::string::npos);
  CHECK(r.warnings[1].find("colour") != std::string::npos);
}

TEST_CASE("bad files give clear errors with the right exit code", "[config]") {
  TempDir dir;
  SettingsSources src;
  src.load_user_config = false;

  SECTION("missing --config file") {
    src.config_file = dir.path / "nope.json";
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(e.exit_code == 1);
    }
  }
  SECTION("invalid JSON in config") {
    src.config_file = dir.write("bad.json", "{ spp: 3 ");
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(e.exit_code == 1);
      CHECK(std::string(e.what()).find("not valid JSON") != std::string::npos);
    }
  }
  SECTION("bad value in config names the file and key") {
    src.config_file = dir.write("c.json", R"({ "max_bounces": 5000 })");
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(std::string(e.what()).find("max_bounces") != std::string::npos);
    }
  }
  SECTION("missing scene is exit code 2") {
    src.cli = {{"scene", (dir.path / "missing.glb").string()}};
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(e.exit_code == 2);
    }
  }
  SECTION(".pen.json without a scene is exit code 2") {
    src.cli = {{"scene", dir.write("x.pen.json", R"({ "render": {} })").string()}};
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(e.exit_code == 2);
    }
  }
  SECTION("unsupported scene type is exit code 2") {
    src.cli = {{"scene", dir.write("scene.blend", "x").string()}};
    try {
      resolveSettings(src);
      FAIL("expected an error");
    } catch (const SettingsError& e) {
      CHECK(e.exit_code == 2);
    }
  }
}

TEST_CASE("the committed test .pen.json loads", "[config]") {
  SettingsSources src;
  src.load_user_config = false;
  src.cli = {{"scene", (kData / "scene.pen.json").string()}};
  const RenderSettings s = resolveSettings(src).settings;
  CHECK(s.res.preset == "2k");
  CHECK(s.spp == 512);
  CHECK(s.camera == "Camera.001");
  CHECK(s.scene.filename() == "empty.gltf");
}
