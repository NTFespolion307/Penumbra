#include <catch2/catch_test_macros.hpp>

#include "core/image/image.h"
#include "core/render/memory_placement.h"
#include "core/scene/scene_loader.h"
#include "worker/device_prep.h"

using namespace pen;

namespace {

constexpr uint64_t MB = 1000 * 1000;

SceneFootprint scene(uint64_t fixed, uint64_t environment, std::vector<TextureSize> textures) {
  SceneFootprint fp;
  fp.fixed = fixed;
  fp.environment = environment;
  fp.textures = std::move(textures);
  return fp;
}

}  // namespace

TEST_CASE("texture sizes halve like mip levels", "[memory]") {
  const TextureSize t{4096, 2048, 4};
  CHECK(t.bytes() == 4096ull * 2048 * 4);
  CHECK(t.bytes(1) == 2048ull * 1024 * 4);
  CHECK(TextureSize{3, 1, 16}.bytes(1) == 1 * 1 * 16);
  CHECK(TextureSize{1, 1, 4}.bytes(5) == 4);
}

TEST_CASE("the tier ladder: VRAM, then RAM, then smaller textures, then skipped", "[memory]") {
  // 100 MB of geometry, 50 MB of environment, two 67 MB textures (4096x4096 RGBA8).
  const SceneFootprint fp = scene(100 * MB, 50 * MB, {{4096, 4096, 4}, {4096, 4096, 4}});
  const uint64_t all = 100 * MB + 50 * MB + 2 * fp.textures[0].bytes();

  SECTION("tier 0: everything fits in VRAM") {
    const Placement p = choosePlacement(fp, {all, 0, true});
    CHECK(p.tier == 0);
    CHECK(p.vram_bytes == all);
    CHECK(p.ram_bytes == 0);
    CHECK_FALSE(p.movable_in_ram);
  }
  SECTION("tier 1: textures and environment to RAM") {
    const Placement p = choosePlacement(fp, {all - 1, all, true});
    CHECK(p.tier == 1);
    CHECK(p.vram_bytes == 100 * MB);
    CHECK(p.ram_bytes == all - 100 * MB);
    CHECK(p.movable_in_ram);
  }
  SECTION("tier 2: the largest textures are halved until they fit") {
    const Placement p = choosePlacement(fp, {150 * MB, 120 * MB, true});
    REQUIRE(p.tier == 2);
    CHECK(p.movable_in_ram);  // 120 MB of RAM is more room than the 50 MB of VRAM left
    REQUIRE(p.halvings.size() == 2);
    CHECK(p.ram_bytes <= 120 * MB);
    CHECK(p.ram_bytes == 50 * MB + fp.textures[0].bytes(p.halvings[0]) + fp.textures[1].bytes(p.halvings[1]));
    // Halving one texture once frees 50 MB: 50 + 17 + 67 = 134 > 120, so both are halved once (50 + 17 + 17).
    CHECK(p.halvings[0] == 1);
    CHECK(p.halvings[1] == 1);
  }
  SECTION("tier 2 in VRAM when VRAM has more room than RAM") {
    const Placement p = choosePlacement(fp, {250 * MB, 10 * MB, true});
    REQUIRE(p.tier == 2);
    CHECK_FALSE(p.movable_in_ram);
    CHECK(p.vram_bytes <= 250 * MB);
    CHECK(p.ram_bytes == 0);
  }
  SECTION("tier 3: the geometry does not fit") {
    const Placement p = choosePlacement(fp, {99 * MB, 1000 * MB, true});
    CHECK(p.tier == 3);
    CHECK(p.reason.find("geometry") != std::string::npos);
  }
  SECTION("tier 3: downsizing forbidden") {
    const Placement p = choosePlacement(fp, {150 * MB, 120 * MB, false});
    CHECK(p.tier == 3);
    CHECK(p.reason.find("--no-texture-downsize") != std::string::npos);
  }
  SECTION("tier 3: the environment alone does not fit anywhere") {
    const Placement p = choosePlacement(fp, {140 * MB, 40 * MB, true});
    CHECK(p.tier == 3);
    CHECK(p.reason.find("environment") != std::string::npos);
  }
}

TEST_CASE("tier 2 is deterministic and halves the biggest texture first", "[memory]") {
  const SceneFootprint fp = scene(0, 0, {{8192, 8192, 4}, {1024, 1024, 4}, {2048, 2048, 16}});
  const MemoryBudgets b{1, 80 * MB, true};
  const Placement a = choosePlacement(fp, b), c = choosePlacement(fp, b);
  REQUIRE(a.tier == 2);
  CHECK(a.halvings == c.halvings);
  CHECK(a.halvings[0] >= 1);  // the 268 MB texture goes first
  CHECK(a.halvings[1] == 0);  // the 4 MB one stays
  CHECK(a.ram_bytes <= 80 * MB);
}

TEST_CASE("the placement log names the tier, VRAM and RAM", "[memory]") {
  const SceneFootprint fp = scene(100 * MB, 0, {{4096, 4096, 4}});
  const MemoryBudgets b{120 * MB, 1000 * MB, true};
  const std::string s = describePlacement(choosePlacement(fp, b), b);
  CHECK(s.rfind("tier 1, scene ", 0) == 0);
  CHECK(s.find("in RAM") != std::string::npos);
}

TEST_CASE("downsized textures average 2x2 blocks and keep odd edges", "[memory]") {
  Texture t;
  t.width = 3;
  t.height = 2;
  t.ldr = {0, 0, 0, 255,  100, 0, 0, 255,  200, 0, 0, 255,   // row 0
           50, 0, 0, 255, 150, 0, 0, 255,  250, 0, 0, 255};  // row 1
  const Texture h = downsized(t, 1);
  REQUIRE(h.width == 1);
  REQUIRE(h.height == 1);
  CHECK(h.ldr[0] == 75);  // (0 + 100 + 50 + 150 + 2) / 4
  CHECK(h.ldr[3] == 255);

  Texture f;
  f.is_float = true;
  f.width = f.height = 4;
  f.hdr.assign(4 * 4 * 4, 2.0f);
  const Texture g = downsized(f, 5);  // more halvings than levels: stops at 1x1
  CHECK(g.width == 1);
  CHECK(g.height == 1);
  CHECK(g.hdr[0] == 2.0f);
  CHECK(downsized(f, 0).hdr == f.hdr);
}

namespace {

// A GPU stand-in: 1 GB of VRAM, a scene of 100 MB geometry and one 67 MB texture, and an upload that runs out of
// memory at the tiers below `fail_up_to + 1`.
class FakeGpu final : public IBackend {
 public:
  explicit FakeGpu(int failUpTo) : fail_up_to_(failUpTo) {}
  DeviceInfo info() const override {
    DeviceInfo d;
    d.name = "fake GPU";
    d.backend = BackendKind::Vulkan;
    return d;
  }
  uint64_t vramBudget() override { return 1000 * MB; }
  SceneFootprint measure(const Scene&) override {
    SceneFootprint fp;
    fp.fixed = 100 * MB;
    fp.textures = {{4096, 4096, 4}};
    return fp;
  }
  void upload(const Scene&, const Placement& p) override {
    tiers.push_back(p.tier);
    if (int(p.tier) <= fail_up_to_) throw OutOfMemoryError("fake: out of memory");
  }
  void prepare(const FrameSetup&) override {}
  void render(const WorkItem&, TileResult&) override {}
  void cancel() override {}
  std::vector<uint32_t> tiers;

 private:
  int fail_up_to_;
};

}  // namespace

TEST_CASE("running out of memory while taking the scene moves the device down the ladder", "[memory]") {
  std::vector<std::string> warnings;
  const Scene scene = loadGltf(std::string(PEN_TEST_DATA_DIR) + "/empty.gltf", warnings);
  RenderSettings settings;
  settings.ram_limit = 64ull << 30;  // plenty of pinned RAM, whatever this machine has

  SECTION("tier 0 fails, tier 1 works") {
    std::vector<std::unique_ptr<IBackend>> devices;
    devices.push_back(std::make_unique<FakeGpu>(0));
    auto* gpu = static_cast<FakeGpu*>(devices[0].get());
    const auto ready = prepareDevices(scene, settings, devices);
    REQUIRE(ready.size() == 1);
    CHECK_FALSE(ready[0].skipped);
    CHECK(ready[0].memory_tier == 1);
    CHECK(gpu->tiers == std::vector<uint32_t>{0, 1});
  }
  SECTION("every tier fails: the device is skipped, nothing crashes") {
    std::vector<std::unique_ptr<IBackend>> devices;
    devices.push_back(std::make_unique<FakeGpu>(2));
    CHECK_THROWS_AS(prepareDevices(scene, settings, devices), BackendError);  // no device left
  }
}
