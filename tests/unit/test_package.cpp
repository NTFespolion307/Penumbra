#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include "core/package/package.h"
#include "core/scene/scene_loader.h"
#include "core/system/hash.h"
#include "worker/scene_cache.h"

using namespace pen;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / ("penumbra-test-" + name)) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void writeFile(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

std::string randomBytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string s(n, '\0');
  for (auto& c : s) c = static_cast<char>(rng());
  return s;
}

// A glTF with an external buffer and an image in a subfolder; the HDRI sits in a sibling folder.
RenderSettings gltfScene(const fs::path& root, size_t textureBytes) {
  writeFile(root / "scene" / "model.bin", std::string(64, '\x01'));
  writeFile(root / "scene" / "tex" / "wood.png", randomBytes(textureBytes, 1));
  writeFile(root / "scene" / "model.gltf",
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":64,"uri":"model.bin"}],)"
            R"("images":[{"uri":"tex/wood.png"}],"textures":[{"source":0}]})");
  writeFile(root / "hdri" / "sky.hdr", randomBytes(1000, 2));
  RenderSettings s;
  s.scene = root / "scene" / "model.gltf";
  s.environment.hdri = root / "hdri" / "sky.hdr";
  return s;
}

}  // namespace

TEST_CASE("glTF and OBJ scenes list every file they need", "[package]") {
  TempDir t("collect");
  const RenderSettings s = gltfScene(t.path, 100);
  const auto files = collectSceneFiles(s);
  REQUIRE(files.size() == 4);
  CHECK(files[0].filename() == "model.gltf");
  CHECK(files[1].filename() == "model.bin");
  CHECK(files[2].filename() == "wood.png");
  CHECK(files[3].filename() == "sky.hdr");

  writeFile(t.path / "obj" / "box.obj", "mtllib box.mtl\nv 0 0 0\n");
  writeFile(t.path / "obj" / "box.mtl", "newmtl a\nmap_Kd -s 1 1 1 maps/a.png\nmap_Bump unused.png\n");
  writeFile(t.path / "obj" / "maps" / "a.png", "png");
  RenderSettings o;
  o.scene = t.path / "obj" / "box.obj";
  const auto objFiles = collectSceneFiles(o);
  REQUIRE(objFiles.size() == 3);
  CHECK(objFiles[1].filename() == "box.mtl");
  CHECK(objFiles[2].filename() == "a.png");
}

TEST_CASE("a missing texture fails packaging with its name", "[package]") {
  TempDir t("missing");
  const RenderSettings s = gltfScene(t.path, 100);
  fs::remove(t.path / "scene" / "tex" / "wood.png");
  try {
    buildPackage(s);
    FAIL("expected SceneError");
  } catch (const SceneError& e) {
    CHECK(std::string(e.what()).find("wood.png") != std::string::npos);
  }
}

TEST_CASE("packages describe files relative to their common folder and survive the manifest", "[package]") {
  TempDir t("build");
  const RenderSettings s = gltfScene(t.path, ScenePackage::kChunkBytes + 12345);  // 2 chunks
  const ScenePackage pkg = buildPackage(s);
  CHECK(pkg.entry == "scene/model.gltf");
  CHECK(pkg.hdri == "hdri/sky.hdr");
  REQUIRE(pkg.files.size() == 4);
  CHECK(pkg.files[2].path == "scene/tex/wood.png");
  CHECK(pkg.chunkCount() == 2);

  // The chunks are the files laid end to end.
  std::string all;
  for (const auto& src : pkg.sources) {
    std::ifstream in(src, std::ios::binary);
    all += std::string(std::istreambuf_iterator<char>(in), {});
  }
  std::string chunks;
  for (uint32_t i = 0; i < pkg.chunkCount(); ++i) {
    const auto c = readPackageChunk(pkg, i);
    CHECK(hash64(c) == pkg.chunk_hashes[i]);
    chunks.append(c.begin(), c.end());
  }
  CHECK(chunks == all);

  const ScenePackage back = ScenePackage::fromManifest(pkg.manifest());
  CHECK(back.hash() == pkg.hash());
  CHECK(back.sources.empty());
  const RenderSettings moved = back.relocate(s, t.path / "cache");
  CHECK(moved.scene == t.path / "cache" / "scene" / "model.gltf");
  CHECK(moved.environment.hdri == t.path / "cache" / "hdri" / "sky.hdr");
  // Settings are not part of the package: other settings, same package.
  RenderSettings other = s;
  other.spp = 7;
  CHECK(buildPackage(other).hash() == pkg.hash());
}

TEST_CASE("manifests with unsafe paths are refused", "[package]") {
  TempDir t("unsafe");
  const ScenePackage pkg = buildPackage(gltfScene(t.path, 100));
  std::string m = pkg.manifest();
  const std::string from = "\"scene/model.bin\"";
  m.replace(m.find(from), from.size(), "\"../../evil.bin\"");
  CHECK_THROWS_AS(ScenePackage::fromManifest(m), SceneError);
  CHECK_THROWS_AS(ScenePackage::fromManifest("{not json"), SceneError);
}

TEST_CASE("the scene cache downloads, resumes after an interruption, and finds the result", "[package]") {
  TempDir t("cache");
  const RenderSettings s = gltfScene(t.path, 2 * ScenePackage::kChunkBytes + 99);  // 3 chunks
  const ScenePackage src = buildPackage(s);
  const ScenePackage pkg = ScenePackage::fromManifest(src.manifest());  // what a worker sees
  const uint64_t hash = pkg.hash();
  {
    SceneCache cache(t.path / "cache", 1ull << 30);
    CHECK_FALSE(cache.find(pkg, hash).has_value());
    CHECK_FALSE(cache.cannotHold(pkg).has_value());
    CHECK(cache.beginDownload(pkg, hash) == 0);
    cache.addChunk(0, readPackageChunk(src, 0));
    CHECK_THROWS_AS(cache.addChunk(2, readPackageChunk(src, 2)), FileError);  // out of order
    auto bad = readPackageChunk(src, 1);
    bad[10] ^= 1;
    CHECK_THROWS_AS(cache.addChunk(1, bad), FileError);  // corrupt
    // The worker stops here (killed): one good chunk is on disk.
  }
  SceneCache cache(t.path / "cache", 1ull << 30);
  CHECK(cache.beginDownload(pkg, hash) == 1);
  cache.addChunk(1, readPackageChunk(src, 1));
  cache.addChunk(2, readPackageChunk(src, 2));
  REQUIRE(cache.downloadComplete());
  const fs::path folder = cache.finishDownload();
  CHECK(hashFile(folder / "scene" / "tex" / "wood.png") == src.files[2].hash);
  CHECK(cache.find(pkg, hash) == folder);
  CHECK_FALSE(fs::exists(t.path / "cache" / (toHex(hash) + ".partial")));

  SceneCache tiny(t.path / "cache2", 1000);
  CHECK(tiny.cannotHold(pkg).has_value());
}
