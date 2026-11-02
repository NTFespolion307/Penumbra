#include <catch2/catch_test_macros.hpp>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/film/film.h"
#include "core/film/session.h"
#include "core/image/image_io.h"
#include "core/output/writers.h"
#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/settings/serialize.h"
#include "core/system/file.h"
#include "core/system/hash.h"

using namespace pen;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("penumbra-film-" + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

TileResult result(const TileRect& r, uint32_t s0, uint32_t s1, Vec3f value) {
  TileResult t;
  t.rect = r;
  t.sample_begin = s0;
  t.sample_end = s1;
  t.mean.assign(r.pixels(), value);
  return t;
}

}  // namespace

TEST_CASE("film merge is a sample-weighted mean, in RAM and on disk", "[film]") {
  for (bool ram : {true, false}) {
    TempDir dir;
    const FilmLayout layout = FilmLayout::make(40, 30, 16);  // 3 x 2 tiles, ragged edges
    auto film = Film::create(dir.path / "a.penfilm", layout, {1, 2}, ram);
    CHECK(film->inRam() == ram);
    film->merge(result({0, 0, 40, 30}, 0, 3, {1, 2, 3}), 3);
    film->merge(result({20, 10, 40, 30}, 3, 4, {5, 6, 7}), 4);  // spans several tiles
    std::vector<Vec3f> out;
    std::vector<uint32_t> counts;
    film->readRect({0, 0, 40, 30}, out, &counts);
    CHECK(out[0] == Vec3f(1, 2, 3));  // a single result is stored bit for bit
    CHECK(counts[0] == 3);
    const size_t i = size_t(15) * 40 + 25;
    CHECK(counts[i] == 4);
    CHECK(std::fabs(out[i].x - 2.0f) < 1e-6f);  // (1*3 + 5*1) / 4
    CHECK(film->tilesWithData() == 6);
  }
}

TEST_CASE("film survives a checkpoint and reopen", "[film]") {
  TempDir dir;
  const fs::path path = dir.path / "b.penfilm";
  const FilmLayout layout = FilmLayout::make(64, 64, 32);
  {
    auto film = Film::create(path, layout, {11, 22}, false);
    film->merge(result(layout.tileRect(0), 0, 8, {0.5f, 0.25f, 0.125f}), 8);
    film->merge(result(layout.tileRect(3), 0, 8, {2, 2, 2}), 8);
    film->checkpoint();
  }
  auto film = Film::open(path, false, true);
  CHECK(film->layout() == layout);
  CHECK(film->identity() == FilmIdentity{11, 22});
  CHECK(film->tileHasData(0));
  CHECK_FALSE(film->tileHasData(1));
  CHECK(film->tileHasData(3));
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  film->readTile(3, mean, count);
  CHECK(mean[5] == Vec3f(2, 2, 2));
  CHECK(count[5] == 8);
}

TEST_CASE("a crash after a checkpoint loses only the tiles merged since", "[film]") {
  TempDir dir;
  const fs::path path = dir.path / "c.penfilm";
  const FilmLayout layout = FilmLayout::make(64, 64, 32);
  uint64_t sizeAtCheckpoint = 0;
  {
    auto film = Film::create(path, layout, {}, false);
    film->merge(result(layout.tileRect(0), 0, 4, {1, 1, 1}), 4);
    film->checkpoint();
    sizeAtCheckpoint = fs::file_size(path);
    film->merge(result(layout.tileRect(1), 0, 4, {9, 9, 9}), 4);  // never checkpointed: the "crash"
    film->flushTile(1);  // even written to disk, it is not in the saved index
  }
  CHECK(fs::file_size(path) > sizeAtCheckpoint);
  auto film = Film::open(path, /*writable=*/true, false);
  CHECK(fs::file_size(path) == sizeAtCheckpoint);  // the unconfirmed block is gone
  CHECK(film->tileHasData(0));
  CHECK_FALSE(film->tileHasData(1));
  film->merge(result(layout.tileRect(1), 0, 4, {3, 3, 3}), 4);  // rendering continues normally
  film->checkpoint();
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  film->readTile(1, mean, count);
  CHECK(mean[0] == Vec3f(3, 3, 3));
}

TEST_CASE("a tile rewritten many times never grows the file past two slots", "[film]") {
  TempDir dir;
  const fs::path path = dir.path / "slots.penfilm";
  const FilmLayout layout = FilmLayout::make(64, 64, 32);  // 4 tiles
  {
    auto film = Film::create(path, layout, {}, /*keepInRam=*/true);
    for (uint32_t pass = 0; pass < 10; ++pass) {
      for (uint32_t t = 0; t < 4; ++t)
        film->merge(result(layout.tileRect(t), pass * 2, pass * 2 + 2, {float(pass), 0, 0}), pass * 2 + 2);
      film->checkpoint();
    }
  }
  CHECK(fs::file_size(path) <= Film::fileBytes(layout) * 2);
  auto film = Film::open(path, false, false);
  CHECK(film->tileSamples(2) == 20);
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  film->readTile(2, mean, count);
  CHECK(count[0] == 20);
  CHECK(std::fabs(mean[0].x - 4.5f) < 1e-5f);  // mean of 0..9, each twice
}

TEST_CASE("a crash between checkpoints keeps the last saved version of a rewritten tile", "[film]") {
  TempDir dir;
  const fs::path path = dir.path / "cow.penfilm";
  const FilmLayout layout = FilmLayout::make(32, 32, 32);
  {
    auto film = Film::create(path, layout, {}, true);
    film->merge(result(layout.tileRect(0), 0, 2, {1, 1, 1}), 2);
    film->checkpoint();
    film->merge(result(layout.tileRect(0), 2, 4, {3, 3, 3}), 4);
    film->checkpoint();
    film->merge(result(layout.tileRect(0), 4, 6, {5, 5, 5}), 6);  // lost in the "crash"
  }
  auto film = Film::open(path, true, false);
  CHECK(film->tileSamples(0) == 4);
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  film->readTile(0, mean, count);
  CHECK(mean[0] == Vec3f(2, 2, 2));
  CHECK(count[0] == 4);
}

TEST_CASE("corrupt tiles are detected", "[film]") {
  TempDir dir;
  const fs::path path = dir.path / "d.penfilm";
  const FilmLayout layout = FilmLayout::make(32, 32, 32);
  {
    auto film = Film::create(path, layout, {}, false);
    film->merge(result(layout.tileRect(0), 0, 1, {1, 1, 1}), 1);
    film->checkpoint();
  }
  {
    BinaryFile f(path, BinaryFile::Mode::ReadWrite);
    const uint8_t junk = 0x5a;
    f.writeAt(f.size() - 7, &junk, 1);
  }
  auto film = Film::open(path, false, false);
  std::vector<Vec3f> mean;
  std::vector<uint32_t> count;
  CHECK_THROWS_AS(film->readTile(0, mean, count), FileError);
  CHECK_THROWS_AS(Film::open(dir.path / "missing.penfilm", false, false), FileError);
}

TEST_CASE("settings survive a JSON round trip", "[film]") {
  RenderSettings s;
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"res", "3000x2000"}, {"scale", "0.5"}, {"spp", "777"}, {"max_bounces", "9"}, {"max_glossy", "3"},
           {"integrator", "ao"}, {"no_rr", "true"}, {"seed", "42"}, {"time_limit", "1h5m"}, {"backend", "cpu"},
           {"devices", "cpu"}, {"threads", "6"}, {"tile_size", "128"}, {"vram_limit", "6G"}, {"output", "x/out.exr"},
           {"bit_depth", "32"}, {"tonemap", "filmic"}, {"exposure", "-0.75"}, {"aov", "albedo,depth"},
           {"checkpoint", "2m"}, {"region", "10,20,300,400"}, {"sample_range", "4-99"}, {"port", "7421"},
           {"upload_limit", "10M"}, {"worker_name", "box"}, {"stay", "true"}, {"cache_size", "40G"}}) {
    applySetting(s, *findSetting(key), value, key);
  }
  s.environment.color = {0.5, 0.25, 1};
  s.camera_overrides.fstop = 2.8;
  finalizeSettings(s);
  const nlohmann::json j = settingsToJson(s);
  for (const auto& spec : settingSchema()) {
    INFO(std::string(spec.key));
    CHECK(j.contains(std::string(spec.key)));
  }
  CHECK(settingsFromJson(j) == s);
}

TEST_CASE("the pixel fingerprint ignores output-only settings", "[film]") {
  RenderSettings a;
  finalizeSettings(a);
  RenderSettings b = a;
  b.output = "elsewhere.exr";
  b.exposure = 2;
  b.threads = 3;
  CHECK(pixelSettingsHash(a) == pixelSettingsHash(b));
  b.spp = 256;
  CHECK(pixelSettingsHash(a) != pixelSettingsHash(b));
  CHECK(isPixelSetting("spp"));
  CHECK_FALSE(isPixelSetting("output"));
}

TEST_CASE("session files round-trip and are written atomically", "[film]") {
  TempDir dir;
  Session s;
  s.app_version = "0.1.0";
  s.git_hash = "abc";
  s.settings.spp = 99;
  finalizeSettings(s.settings);
  s.film = dir.path / "r.penfilm";
  s.scene_hash = 0x1234;
  s.settings_hash = 0xfeedfacecafebeefull;
  s.tiles_done = 3;
  s.tiles_total = 10;
  s.created = s.updated = localTimestamp();
  const fs::path path = dir.path / "r.session.json";
  s.save(path);
  s.save(path);  // replacing an existing file works
  CHECK_FALSE(fs::exists(dir.path / "r.session.json.tmp"));
  const Session t = Session::load(path);
  CHECK(t.settings == s.settings);
  CHECK(t.settings_hash == s.settings_hash);
  CHECK(t.tiles_done == 3);
  CHECK(sessionPathFor(dir.path / "r.penfilm") == path);
}

TEST_CASE("every output format writes a readable file", "[film][output]") {
  TempDir dir;
  constexpr uint32_t W = 300, H = 270;  // two bands, the second partial
  const auto value = [](uint32_t x, uint32_t y, int c) { return (x * 3 + y * 5 + c * 50) % 256 / 255.0f; };
  struct Case {
    ImageFormat format;
    uint32_t depth;
    const char* name;
  };
  for (const Case& c : {Case{ImageFormat::Exr, 32, "a.exr"}, Case{ImageFormat::Exr, 16, "b.exr"},
                        Case{ImageFormat::Png, 8, "c.png"}, Case{ImageFormat::Png, 16, "d.png"},
                        Case{ImageFormat::Jpeg, 8, "e.jpg"}, Case{ImageFormat::Tiff, 16, "f.tiff"},
                        Case{ImageFormat::Tiff, 32, "g.tiff"}, Case{ImageFormat::Webp, 8, "h.webp"}}) {
    INFO(c.name);
    const fs::path path = dir.path / c.name;
    auto writer = makeImageWriter(path, c.format, c.depth, W, H, {{"Software", "test"}});
    for (uint32_t y0 = 0; y0 < H; y0 += ImageWriter::kBandRows) {
      const uint32_t rows = std::min(ImageWriter::kBandRows, H - y0);
      std::vector<float> rgb(size_t(W) * rows * 3);
      for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < W; ++x)
          for (int ch = 0; ch < 3; ++ch) rgb[(size_t(y) * W + x) * 3 + ch] = value(x, y0 + y, ch);
      writer->writeBand(y0, rows, rgb);
    }
    writer->finish();
    REQUIRE(fs::file_size(path) > 100);
    if (c.format == ImageFormat::Exr || c.format == ImageFormat::Png || c.format == ImageFormat::Jpeg) {
      const ImageRGB img = loadImageRGB(path);
      CHECK(img.width == W);
      CHECK(img.height == H);
      // EXR is linear (exact for float); PNG/JPEG hold display values, which loadImageRGB linearizes again.
      const float stored = img.at(123, 261).x;
      const float expected = c.format == ImageFormat::Exr ? value(123, 261, 0) : srgbToLinear(value(123, 261, 0));
      const float tol = c.format == ImageFormat::Exr ? (c.depth == 32 ? 0.0f : 1e-3f)
                                                     : (c.format == ImageFormat::Jpeg ? 0.05f : 2e-3f);
      CHECK(std::fabs(stored - expected) <= tol);
    } else {
      std::ifstream in(path, std::ios::binary);
      char magic[12] = {};
      in.read(magic, 12);
      if (c.format == ImageFormat::Tiff) CHECK((std::string(magic, 2) == "II" || std::string(magic, 2) == "MM"));
      if (c.format == ImageFormat::Webp) CHECK(std::string(magic + 8, 4) == "WEBP");
    }
  }
}

namespace {

// Decodes a PNG with libpng: the raw samples (16-bit as big-endian pairs), the size, depth and the text chunks.
struct DecodedPng {
  uint32_t width = 0, height = 0;
  int depth = 0, colour = -1;
  std::vector<uint8_t> samples;
  std::map<std::string, std::string> text;
};

DecodedPng decodePng(const fs::path& path) {
  DecodedPng out;
#if defined(_WIN32)
  FILE* fp = _wfopen(path.wstring().c_str(), L"rb");
#else
  FILE* fp = std::fopen(path.c_str(), "rb");
#endif
  REQUIRE(fp);
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png_create_info_struct(png);
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    FAIL("libpng could not decode " << path.string());
  }
  png_init_io(png, fp);
  png_read_info(png, info);
  out.width = png_get_image_width(png, info);
  out.height = png_get_image_height(png, info);
  out.depth = png_get_bit_depth(png, info);
  out.colour = png_get_color_type(png, info);
  const size_t rowBytes = png_get_rowbytes(png, info);
  out.samples.resize(rowBytes * out.height);
  for (uint32_t y = 0; y < out.height; ++y) png_read_row(png, out.samples.data() + y * rowBytes, nullptr);
  png_read_end(png, info);
  png_textp texts = nullptr;
  const int n = png_get_text(png, info, &texts, nullptr);
  for (int i = 0; i < n; ++i) out.text[texts[i].key] = std::string(texts[i].text, texts[i].text_length);
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(fp);
  return out;
}

}  // namespace

TEST_CASE("PNG output decodes to exactly the quantized values, bands deflated in parallel", "[film][output]") {
  TempDir dir;
  struct Case {
    uint32_t w, h, depth;
  };
  // Two bands with a partial one, a single exact band, a 1-pixel-wide image of three bands, and 16 bits.
  for (const Case c : {Case{300, 270, 8}, Case{1000, 256, 8}, Case{1, 600, 16}, Case{257, 513, 16}}) {
    INFO(c.w << "x" << c.h << " " << c.depth << "-bit");
    // Smooth gradients, flat areas and noise, so every filter type gets chosen somewhere.
    std::mt19937 rng(c.w * 31 + c.h);
    const auto value = [&](uint32_t x, uint32_t y, int ch) -> float {
      if (y % 64 < 16) return 0.25f;
      if (y % 64 < 40) return float((x + 2 * y + 40 * ch) % 997) / 996.0f;
      return float(rng() % 70001) / 70000.0f * 1.2f - 0.1f;  // outside [0,1] too: clamped
    };
    std::vector<float> all(size_t(c.w) * c.h * 3);
    for (uint32_t y = 0; y < c.h; ++y)
      for (uint32_t x = 0; x < c.w; ++x)
        for (int ch = 0; ch < 3; ++ch) all[(size_t(y) * c.w + x) * 3 + ch] = value(x, y, ch);
    const fs::path path = dir.path / "p.png";
    auto writer = makeImageWriter(path, ImageFormat::Png, c.depth, c.w, c.h, {{"Software", "test"}, {"Scene", "a b"}});
    for (uint32_t y0 = 0; y0 < c.h; y0 += ImageWriter::kBandRows) {
      const uint32_t rows = std::min(ImageWriter::kBandRows, c.h - y0);
      writer->writeBand(y0, rows, std::span<const float>(all.data() + size_t(y0) * c.w * 3, size_t(rows) * c.w * 3));
    }
    writer->finish();

    const DecodedPng png = decodePng(path);
    REQUIRE(png.width == c.w);
    REQUIRE(png.height == c.h);
    CHECK(png.depth == int(c.depth));
    CHECK(png.colour == PNG_COLOR_TYPE_RGB);
    CHECK(png.text.at("Software") == "test");
    CHECK(png.text.at("Scene") == "a b");
    const float maxV = c.depth == 16 ? 65535.0f : 255.0f;
    size_t mismatches = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      const float v = std::clamp(all[i], 0.0f, 1.0f);
      const uint32_t expected = static_cast<uint32_t>(std::lround(v * maxV));
      const uint32_t got = c.depth == 16 ? (uint32_t(png.samples[i * 2]) << 8 | png.samples[i * 2 + 1]) : png.samples[i];
      if (got != expected) ++mismatches;
    }
    CHECK(mismatches == 0);
  }
}

TEST_CASE("EXR crops decode only the requested rectangle", "[film][output]") {
  TempDir dir;
  const fs::path path = dir.path / "crop.exr";
  auto writer = makeImageWriter(path, ImageFormat::Exr, 32, 600, 300, {});
  for (uint32_t y0 = 0; y0 < 300; y0 += ImageWriter::kBandRows) {
    const uint32_t rows = std::min(ImageWriter::kBandRows, 300 - y0);
    std::vector<float> rgb(size_t(600) * rows * 3);
    for (uint32_t y = 0; y < rows; ++y)
      for (uint32_t x = 0; x < 600; ++x) rgb[(size_t(y) * 600 + x) * 3] = float(x + (y0 + y) * 1000);
    writer->writeBand(y0, rows, rgb);
  }
  writer->finish();
  const ImageRGB c = loadImageRGB(path, ImageCrop{500, 250, 520, 290});
  CHECK(c.width == 20);
  CHECK(c.height == 40);
  CHECK(c.at(0, 0).x == 500.0f + 250 * 1000);
  CHECK(c.at(19, 39).x == 519.0f + 289 * 1000);
}

TEST_CASE("file helpers", "[film]") {
  TempDir dir;
  CHECK(freeDiskBytes(dir.path) > 0);
  writeFileAtomically(dir.path / "x.txt", "hello");
  writeFileAtomically(dir.path / "x.txt", "world");
  std::ifstream in(dir.path / "x.txt");
  std::string s;
  in >> s;
  CHECK(s == "world");
  CHECK(hash64(std::string_view("abc")) != hash64(std::string_view("abd")));
  CHECK(fromHex(toHex(0x0123456789abcdefull)) == 0x0123456789abcdefull);
}
