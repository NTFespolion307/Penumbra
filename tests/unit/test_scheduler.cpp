#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

#include "controller/scheduler.h"
#include "core/film/film.h"

using namespace pen;

TEST_CASE("chunk sizes depend only on tile size and sample range", "[scheduler]") {
  constexpr uint64_t k22 = uint64_t(1) << 22;
  CHECK(chunkSamples(256 * 256, 1024, k22) == 64);    // 2^22 / 65536
  CHECK(chunkSamples(256 * 256, 40, k22) == 40);      // capped by the range
  CHECK(chunkSamples(1280 * 720, 1024, k22) == 4);    // a full-frame tile below 1080p
  CHECK(chunkSamples(5000 * 5000, 1024, k22) == 1);   // never below 1
  CHECK(chunkSamples(1, kUnboundedSamples, k22) == (1u << 22));
  // The default budget (D-083): 1,024 samples for a 256 x 256 tile.
  CHECK(chunkSamples(256 * 256, 4096, uint64_t(1) << 26) == 1024);
}

TEST_CASE("every pixel and sample is covered exactly once, centre first", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(2560, 1440, 256);
  Scheduler s(layout, 0, 200, std::nullopt, PassOrder::DepthFirst);
  CHECK(s.tileCount() == 60);
  std::vector<uint32_t> covered(size_t(2560) * 1440, 0);
  std::optional<WorkItem> first;
  std::set<uint64_t> ids;
  while (auto item = s.next()) {
    if (!first) first = item;
    CHECK(ids.insert(item->id).second);
    for (uint32_t y = item->rect.y0; y < item->rect.y1; ++y)
      for (uint32_t x = item->rect.x0; x < item->rect.x1; ++x) covered[size_t(y) * 2560 + x] += item->samples();
  }
  for (uint32_t c : covered) REQUIRE(c == 200);
  REQUIRE(first);
  CHECK(first->rect.x0 <= 1280);
  CHECK(first->rect.x1 >= 1280);
  CHECK(s.totalSamples() == uint64_t(2560) * 1440 * 200);
}

TEST_CASE("chunks have fixed boundaries", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(256, 256, 256);  // below 1080p: filmTileSize would also say 256
  Scheduler s(layout, 0, 200, std::nullopt, PassOrder::DepthFirst);
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  while (auto item = s.next()) ranges.emplace_back(item->sample_begin, item->sample_end);
  CHECK(ranges == std::vector<std::pair<uint32_t, uint32_t>>{{0, 64}, {64, 128}, {128, 192}, {192, 200}});
  Scheduler r(layout, 100, 228, std::nullopt, PassOrder::DepthFirst);  // --sample-range 100-227
  CHECK(r.next()->sample_begin == 100);
}

TEST_CASE("breadth-first gives every tile a chunk before any tile gets two", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(1024, 1024, 256);  // 16 tiles, 64-sample chunks
  Scheduler b(layout, 0, 128, std::nullopt, PassOrder::BreadthFirst);
  for (int i = 0; i < 16; ++i) CHECK(b.next()->chunk == 0);
  for (int i = 0; i < 16; ++i) CHECK(b.next()->chunk == 1);
  CHECK_FALSE(b.next().has_value());

  Scheduler d(layout, 0, 128, std::nullopt, PassOrder::DepthFirst);
  const WorkItem a = *d.next(), c = *d.next();
  CHECK(a.tile == c.tile);
  CHECK(a.chunk == 0);
  CHECK(c.chunk == 1);
}

TEST_CASE("small frames are one tile split by samples", "[scheduler]") {
  const uint32_t ts = filmTileSize(1280, 720, 256);
  CHECK(ts == 1280);
  Scheduler s(FilmLayout::make(1280, 720, ts), 0, 16, std::nullopt, PassOrder::BreadthFirst);
  CHECK(s.tileCount() == 1);
  int items = 0;
  while (s.next()) ++items;
  CHECK(items == 4);  // 4 samples per chunk
}

TEST_CASE("regions clip tiles", "[scheduler]") {
  Scheduler s(FilmLayout::make(3840, 2160, 256), 0, 1, PixelRect{100, 100, 600, 300}, PassOrder::DepthFirst);
  uint64_t pixels = 0;
  while (auto item = s.next()) {
    CHECK(item->rect.x0 >= 100);
    CHECK(item->rect.y1 <= 300);
    pixels += item->rect.pixels();
  }
  CHECK(pixels == 500 * 200);
}

TEST_CASE("merging in order completes tiles; requeued items come first", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(512, 256, 256);  // 2 tiles
  Scheduler s(layout, 0, 128, std::nullopt, PassOrder::DepthFirst);
  const WorkItem a = *s.next();
  const WorkItem b = *s.next();
  s.requeue(a);
  s.requeue(a);  // requeueing twice queues it once
  CHECK(s.next()->id == a.id);
  CHECK(s.next()->tile != a.tile);  // b's tile is exhausted; the next tile starts
  CHECK(s.mergedChunks(a.tile) == 0);
  s.markMerged(a.tile);
  s.markMerged(a.tile);
  CHECK(s.tileComplete(a.tile));
  CHECK(s.tilesComplete() == 1);
  CHECK_FALSE(s.done());
  (void)b;
}

TEST_CASE("items of several chunks for upload-bound workers (D-116)", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(512, 256, 256);  // 2 tiles, 64-sample chunks (test budget 2^22)
  Scheduler s(layout, 0, 320, std::nullopt, PassOrder::DepthFirst);  // 5 chunks per tile
  const WorkItem a = *s.next(4);
  CHECK(a.chunk == 0);
  CHECK(a.sample_begin == 0);
  CHECK(a.sample_end == 256);
  CHECK(s.chunksIn(a) == 4);
  const WorkItem b = *s.next(4);  // only one chunk of the tile is left
  CHECK(b.tile == a.tile);
  CHECK(b.chunk == 4);
  CHECK(s.chunksIn(b) == 1);
  CHECK(s.next(4)->tile != a.tile);

  s.markMergedUntil(a.tile, a.sample_end);
  CHECK(s.mergedChunks(a.tile) == 4);
  CHECK(s.mergedSamples() == uint64_t(256) * 256 * 256);
  s.markMergedUntil(b.tile, b.sample_end);
  CHECK(s.tileComplete(a.tile));

  // Breadth-first: a tile given 2 chunks sits out the next pass, so passes stay even.
  Scheduler bf(layout, 0, 320, std::nullopt, PassOrder::BreadthFirst);
  const WorkItem first = *bf.next(2);
  CHECK(first.samples() == 128);
  const WorkItem second = *bf.next();
  CHECK(second.tile != first.tile);
  CHECK(second.chunk == 0);
  const WorkItem third = *bf.next();  // pass 1: the first tile already has chunk 1
  CHECK(third.tile == second.tile);
  CHECK(third.chunk == 1);
}

TEST_CASE("restore continues each tile after the samples in the film", "[scheduler]") {
  const FilmLayout layout = FilmLayout::make(1024, 256, 256);  // 4 tiles, 64-sample chunks
  Scheduler s(layout, 0, 256, std::nullopt, PassOrder::BreadthFirst);
  s.restore([](uint32_t tile) { return tile < 2 ? 128u : 64u; });
  CHECK(s.mergedSamples() == uint64_t(256) * 256 * (128 + 128 + 64 + 64));
  // Tiles 2 and 3 (one chunk) catch up first, then everyone moves on together.
  std::set<uint32_t> firstPass;
  for (int i = 0; i < 2; ++i) {
    const WorkItem w = *s.next();
    CHECK(w.chunk == 1);
    firstPass.insert(w.tile);
  }
  CHECK(firstPass == std::set<uint32_t>{2, 3});
  CHECK(s.next()->chunk == 2);
}

TEST_CASE("unbounded renders keep going until stopped", "[scheduler]") {
  Scheduler s(FilmLayout::make(256, 256, 256), 0, kUnboundedSamples, std::nullopt, PassOrder::BreadthFirst);
  for (int i = 0; i < 100; ++i) REQUIRE(s.next().has_value());
  CHECK_FALSE(s.done());
  s.stopNew();
  CHECK_FALSE(s.next().has_value());
}

TEST_CASE("breadth-first hands out every chunk before it reports nothing left", "[scheduler]") {
  // The net.scale_64 frame: 4K, 4,096 spp, 64 chunks per tile (M9, D-112).
  const FilmLayout layout = FilmLayout::make(3840, 2160, 256);  // 135 tiles
  Scheduler s(layout, 0, 4096, std::nullopt, PassOrder::BreadthFirst);
  uint64_t expected = 0;
  for (uint32_t t = 0; t < layout.tileCount(); ++t) expected += s.chunkCount(t);
  uint64_t handed = 0;
  while (s.next()) ++handed;
  CHECK(handed == expected);
}
