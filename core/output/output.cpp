#include "core/output/output.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

#include "core/image/image.h"
#include "core/output/tonemap.h"
#include "core/output/writers.h"

namespace pen {

Vec3f displayEncode(Vec3f linear, float exposureScale, Tonemap mode) { return tonemap(mode, linear * exposureScale); }

TileRect outputRect(const RenderSettings& s) {
  if (s.region) return {s.region->x0, s.region->y0, s.region->x1, s.region->y1};
  return {0, 0, s.outputWidth(), s.outputHeight()};
}

void writeImage(const RowSource& source, uint32_t width, uint32_t height, const RenderSettings& s,
                const ImageMetadata& metadata, std::vector<std::string>& notes) {
  const ImageFormat format = s.format.value_or(ImageFormat::Png);
  const float exposure = std::exp2(static_cast<float>(s.exposure));
  auto writer = makeImageWriter(s.output, format, s.bit_depth, width, height, metadata);
  if (writer->wantsLinear()) {
    if (s.tonemap != Tonemap::None)
      notes.push_back(fmt::format("{}-bit {} output is linear; --tonemap {} applies to display formats only",
                                  s.bit_depth, toString(format), toString(s.tonemap)));
  }

  std::vector<Vec3f> band;
  std::vector<float> rgb;
  for (uint32_t y0 = 0; y0 < height; y0 += ImageWriter::kBandRows) {
    const uint32_t rows = std::min(ImageWriter::kBandRows, height - y0);
    source({0, y0, width, y0 + rows}, band);
    rgb.resize(band.size() * 3);
    for (size_t i = 0; i < band.size(); ++i) {
      const Vec3f v = writer->wantsLinear() ? band[i] * exposure : displayEncode(band[i], exposure, s.tonemap);
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = v[c];
    }
    writer->writeBand(y0, rows, rgb);
  }
  writer->finish();
}

void writeOutput(const Film& film, const RenderSettings& s, const ImageMetadata& metadata,
                 std::vector<std::string>& notes) {
  const TileRect area = outputRect(s);
  const RowSource source = [&](const TileRect& rows, std::vector<Vec3f>& out) {
    film.readRect({area.x0 + rows.x0, area.y0 + rows.y0, area.x0 + rows.x1, area.y0 + rows.y1}, out);
  };
  writeImage(source, area.width(), area.height(), s, metadata, notes);
}

}  // namespace pen
