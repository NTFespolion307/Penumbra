#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/settings/parse.h"

using namespace pen;

TEST_CASE("durations", "[parse]") {
  CHECK(parseDuration("90s") == 90);
  CHECK(parseDuration("10m") == 600);
  CHECK(parseDuration("2h30m") == 9000);
  CHECK(parseDuration("1d2h") == 93600);
  CHECK(parseDuration("45") == 45);
  CHECK(parseDuration(" 2H ") == 7200);
  CHECK_THROWS_AS(parseDuration(""), ValueError);
  CHECK_THROWS_AS(parseDuration("2x"), ValueError);
  CHECK_THROWS_AS(parseDuration("h"), ValueError);
  CHECK_THROWS_AS(parseDuration("1h30"), ValueError);  // trailing number without unit is ambiguous
  CHECK(formatDuration(9000) == "2h30m");
  CHECK(formatDuration(0) == "0s");
}

TEST_CASE("byte sizes", "[parse]") {
  CHECK(parseByteSize("1024") == 1024);
  CHECK(parseByteSize("6G") == 6ull << 30);
  CHECK(parseByteSize("512M") == 512ull << 20);
  CHECK(parseByteSize("20GB") == 20ull << 30);
  CHECK(parseByteSize("1.5g") == 3ull << 29);
  CHECK(parseByteSize("2TiB") == 2ull << 40);
  CHECK_THROWS_AS(parseByteSize("G"), ValueError);
  CHECK_THROWS_AS(parseByteSize("5Q"), ValueError);
  CHECK_THROWS_AS(parseByteSize("5Gx"), ValueError);
  CHECK(formatBytes(20ull << 30) == "20.0 GB");
}

TEST_CASE("numbers and bools", "[parse]") {
  CHECK(parseUint("4294967295", 0, 4294967295ull) == 4294967295ull);
  CHECK_THROWS_AS(parseUint("4294967296", 0, 4294967295ull), ValueError);
  CHECK_THROWS_AS(parseUint("-1", 0, 10), ValueError);
  CHECK_THROWS_AS(parseUint("12abc", 0, 100), ValueError);
  CHECK_THROWS_AS(parseUint("99999999999999999999999", 0, 10), ValueError);
  CHECK_THAT(parseDouble("0.25", 0.01, 1.0), Catch::Matchers::WithinAbs(0.25, 1e-12));
  CHECK_THROWS_AS(parseDouble("2", 0.01, 1.0), ValueError);
  CHECK_THROWS_AS(parseDouble("nan", -1, 1), ValueError);
  CHECK(parseBool("on"));
  CHECK_FALSE(parseBool("False"));
  CHECK_THROWS_AS(parseBool("maybe"), ValueError);
}

TEST_CASE("resolutions", "[parse]") {
  const Resolution r4k = parseResolution("4K");
  CHECK(r4k.width == 3840);
  CHECK(r4k.height == 2160);
  CHECK(r4k.preset == "4k");

  const Resolution custom = parseResolution("3000x2000");
  CHECK(custom.width == 3000);
  CHECK(custom.height == 2000);
  CHECK(custom.preset.empty());

  CHECK(parseResolution("16x16").width == 16);
  CHECK(parseResolution("163840x163840").height == 163840);
  CHECK_THROWS_AS(parseResolution("15x100"), ValueError);
  CHECK_THROWS_AS(parseResolution("163841x100"), ValueError);
  CHECK_THROWS_AS(parseResolution("5k"), ValueError);
  CHECK_THROWS_AS(parseResolution("100"), ValueError);
}

TEST_CASE("regions and sample ranges", "[parse]") {
  const PixelRect r = parseRect("0, 0, 1024, 512");
  CHECK(r == PixelRect{0, 0, 1024, 512});
  CHECK_THROWS_AS(parseRect("10,0,10,5"), ValueError);  // empty
  CHECK_THROWS_AS(parseRect("1,2,3"), ValueError);

  CHECK(parseSampleRange("0-63") == SampleRange{0, 63});
  CHECK(parseSampleRange("5-5") == SampleRange{5, 5});
  CHECK_THROWS_AS(parseSampleRange("10-2"), ValueError);
  CHECK_THROWS_AS(parseSampleRange("10"), ValueError);
}
