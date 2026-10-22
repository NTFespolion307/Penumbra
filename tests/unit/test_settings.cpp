#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "core/settings/schema.h"

using namespace pen;
using nlohmann::json;

namespace {
void set(RenderSettings& s, std::string_view key, const json& v) {
  const SettingSpec* spec = findSetting(key);
  REQUIRE(spec);
  applySetting(s, *spec, v, key);
}
RenderSettings finalized(std::initializer_list<std::pair<std::string_view, json>> values) {
  RenderSettings s;
  for (const auto& [k, v] : values) set(s, k, v);
  finalizeSettings(s);
  return s;
}
}  // namespace

TEST_CASE("schema keys and flags are unique and well formed", "[settings]") {
  std::set<std::string_view> keys, flags;
  for (const auto& spec : settingSchema()) {
    INFO(std::string(spec.key));
    CHECK(keys.insert(spec.key).second);
    CHECK(flags.insert(longFlag(spec)).second);
    CHECK(longFlag(spec).starts_with("--"));
    CHECK(spec.apply != nullptr);
    CHECK_FALSE(spec.help.empty());
    // Flag "--max-bounces" corresponds to key "max_bounces".
    std::string expected(longFlag(spec).substr(2));
    for (char& c : expected)
      if (c == '-') c = '_';
    CHECK(expected == spec.key);
  }
}

TEST_CASE("documented defaults equal the struct defaults", "[settings]") {
  for (const auto& spec : settingSchema()) {
    if (!spec.default_parses) continue;
    INFO(std::string(spec.key) << " default '" << std::string(spec.default_text) << "'");
    RenderSettings s;
    applySetting(s, spec, std::string(spec.default_text), spec.key);
    CHECK(s == RenderSettings{});
  }
}

TEST_CASE("values from JSON files accept native types", "[settings]") {
  RenderSettings s;
  set(s, "spp", 256);
  set(s, "no_rr", true);
  set(s, "exposure", -1.5);
  set(s, "aov", json::array({"albedo", "normal", "albedo"}));
  set(s, "max_diffuse", 4);
  CHECK(s.spp == 256);
  CHECK(s.no_rr);
  CHECK(s.exposure == -1.5);
  CHECK(s.aov == std::vector<std::string>{"albedo", "normal"});
  CHECK(s.max_diffuse == 4u);
  CHECK_THROWS_AS(set(s, "spp", 1.5), SettingsError);
  CHECK_THROWS_AS(set(s, "spp", -3), SettingsError);
  CHECK_THROWS_AS(set(s, "max_bounces", 1025), SettingsError);
  CHECK_THROWS_AS(set(s, "tonemap", "reinhard"), SettingsError);
  CHECK_THROWS_AS(set(s, "devices", "0,gpu1"), SettingsError);
  CHECK_THROWS_AS(set(s, "devices", "0:optix"), SettingsError);
  CHECK_THROWS_AS(set(s, "devices", "cpu:cuda"), SettingsError);
  set(s, "devices", "0:Vulkan, 0:cuda,1,cpu");
  CHECK(s.devices == "0:vulkan,0:cuda,1,cpu");
  const std::vector<DeviceItem> items = parseDeviceList(s.devices);
  REQUIRE(items.size() == 4);
  CHECK((items[0].gpu == 0 && items[0].backend == BackendKind::Vulkan && !items[0].cpu));
  CHECK((items[1].gpu == 0 && items[1].backend == BackendKind::Cuda));
  CHECK((items[2].gpu == 1 && items[2].backend == BackendKind::Auto));
  CHECK(items[3].cpu);
  CHECK_THROWS_AS(set(s, "aov", "albedo,beauty"), SettingsError);
  CHECK_THROWS_AS(set(s, "checkpoint", "5s"), SettingsError);
}

TEST_CASE("spp accepts the full uint32 range", "[settings]") {
  CHECK(finalized({{"spp", "4294967295"}}).spp == 4294967295u);
  CHECK(finalized({{"spp", "1"}}).spp == 1u);
  RenderSettings s;
  CHECK_THROWS_AS(set(s, "spp", "4294967296"), SettingsError);
}

TEST_CASE("spp 0 needs a time limit", "[settings]") {
  CHECK_THROWS_AS(finalized({{"spp", "0"}}), SettingsError);
  CHECK(finalized({{"spp", "0"}, {"time_limit", "2h"}}).time_limit_s == 7200);
}

TEST_CASE("output format comes from the extension or --format", "[settings]") {
  const RenderSettings def = finalized({});
  CHECK(def.format == ImageFormat::Png);
  CHECK(def.bit_depth == 8);

  const RenderSettings exr = finalized({{"output", "out.EXR"}});
  CHECK(exr.format == ImageFormat::Exr);
  CHECK(exr.bit_depth == 16);

  CHECK(finalized({{"output", "a.jpeg"}}).format == ImageFormat::Jpeg);
  CHECK(finalized({{"output", "a.tif"}}).format == ImageFormat::Tiff);

  const RenderSettings appended = finalized({{"output", "render"}, {"format", "exr"}});
  CHECK(appended.output == "render.exr");

  CHECK_THROWS_AS(finalized({{"output", "render"}}), SettingsError);
  CHECK_THROWS_AS(finalized({{"output", "a.bmp"}}), SettingsError);
  CHECK_THROWS_AS(finalized({{"output", "a.png"}, {"format", "exr"}}), SettingsError);
}

TEST_CASE("bit depth per format", "[settings]") {
  CHECK(finalized({{"output", "a.png"}, {"bit_depth", "16"}}).bit_depth == 16);
  CHECK(finalized({{"output", "a.exr"}, {"bit_depth", "32"}}).bit_depth == 32);
  CHECK(finalized({{"output", "a.tiff"}, {"bit_depth", "32"}}).bit_depth == 32);
  CHECK_THROWS_AS(finalized({{"output", "a.jpg"}, {"bit_depth", "16"}}), SettingsError);
  CHECK_THROWS_AS(finalized({{"output", "a.exr"}, {"bit_depth", "8"}}), SettingsError);
  RenderSettings s;
  CHECK_THROWS_AS(set(s, "bit_depth", "12"), SettingsError);
}

TEST_CASE("format size limits are enforced before rendering", "[settings]") {
  // WebP: 16383 px per side.
  CHECK_NOTHROW(finalized({{"output", "a.webp"}, {"res", "15k"}}));
  CHECK_THROWS_AS(finalized({{"output", "a.webp"}, {"res", "20k"}}), SettingsError);
  CHECK_NOTHROW(finalized({{"output", "a.webp"}, {"res", "20k"}, {"scale", "0.5"}}));
  // JPEG: 65500 px per side (libjpeg-turbo).
  CHECK_NOTHROW(finalized({{"output", "a.jpg"}, {"res", "60k"}}));
  CHECK_THROWS_AS(finalized({{"output", "a.jpg"}, {"res", "80k"}}), SettingsError);
  // EXR, PNG, TIFF: every preset.
  CHECK_NOTHROW(finalized({{"output", "a.exr"}, {"res", "160k"}}));
  CHECK_NOTHROW(finalized({{"output", "a.png"}, {"res", "160k"}}));
  CHECK_NOTHROW(finalized({{"output", "a.tiff"}, {"res", "160k"}}));
}

TEST_CASE("scale shrinks the output size", "[settings]") {
  const RenderSettings s = finalized({{"res", "4k"}, {"scale", "0.25"}});
  CHECK(s.outputWidth() == 960);
  CHECK(s.outputHeight() == 540);
  const RenderSettings tiny = finalized({{"res", "120p"}, {"scale", "0.01"}});
  CHECK(tiny.outputWidth() == 2);
  CHECK(tiny.outputHeight() == 1);
}

TEST_CASE("region and sample range must fit", "[settings]") {
  CHECK_NOTHROW(finalized({{"res", "1080p"}, {"region", "0,0,1920,1080"}}));
  CHECK_THROWS_AS(finalized({{"res", "1080p"}, {"region", "0,0,1921,1080"}}), SettingsError);
  CHECK_NOTHROW(finalized({{"spp", "64"}, {"sample_range", "0-63"}}));
  CHECK_THROWS_AS(finalized({{"spp", "64"}, {"sample_range", "0-64"}}), SettingsError);
  CHECK_FALSE(finalized({{"region", "none"}}).region.has_value());
}

TEST_CASE("optional bounce caps", "[settings]") {
  CHECK_FALSE(finalized({}).max_glossy.has_value());
  CHECK(finalized({{"max_glossy", "0"}}).max_glossy == 0u);
  CHECK_FALSE(finalized({{"max_glossy", "8"}, {"max_glossy", "auto"}}).max_glossy.has_value());
}
