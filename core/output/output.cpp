#include "core/output/output.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <fmt/format.h>

#include "core/image/image.h"
#include "core/output/denoise.h"
#include "core/output/tonemap.h"
#include "core/output/writers.h"
#include "core/system/paths.h"

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

std::filesystem::path aovOutputPath(const std::filesystem::path& output, Pass pass) {
  return output.parent_path() / (pathToUtf8(output.stem()) + "." + passName(pass) + ".exr");
}

void writeAov(const Film& film, Pass pass, const RenderSettings& s, const ImageMetadata& metadata,
              std::vector<std::string>& notes) {
  RenderSettings a = s;
  a.output = aovOutputPath(s.output, pass);
  a.format = ImageFormat::Exr;
  a.bit_depth = pass == Pass::Depth ? 32 : 16;
  a.exposure = 0;
  a.tonemap = Tonemap::None;
  const TileRect area = outputRect(s);
  const RowSource source = [&](const TileRect& rows, std::vector<Vec3f>& out) {
    film.readRect({area.x0 + rows.x0, area.y0 + rows.y0, area.x0 + rows.x1, area.y0 + rows.y1}, out);
  };
  std::vector<std::string> ignored;
  writeImage(source, area.width(), area.height(), a, metadata, ignored);
  notes.push_back("wrote the " + std::string(passName(pass)) + " AOV: " + pathToUtf8(a.output));
}

void writeOutput(const Film& film, const RenderSettings& s, const ImageMetadata& metadata,
                 std::vector<std::string>& notes, const Film* albedo, const Film* normal) {
  const TileRect area = outputRect(s);
  RowSource source = [&](const TileRect& rows, std::vector<Vec3f>& out) {
    film.readRect({area.x0 + rows.x0, area.y0 + rows.y0, area.x0 + rows.x1, area.y0 + rows.y1}, out);
  };
  std::unique_ptr<OidnDenoiser> denoiser;
  if (s.denoise == Denoiser::Oidn) {
    if (!albedo || !normal) {
      notes.push_back("not denoised: the albedo and normal passes are missing");
    } else {
      denoiser = std::make_unique<OidnDenoiser>();
      // docs/GUIDE.md "Output": each band in tiles of up to 1024 px wide, each read with a border (clipped to the
      // image) and denoised on its own, so even a 160K image never sits in memory whole. No seams (D-128): one
      // input scale for the whole image (from about 512 rows of it), and the border is OIDN's tile overlap (at
      // least the guide's 32 px), rounded up to its tile alignment.
      std::vector<Vec3f> sample, row;
      const uint32_t step = std::max(1u, area.height() / 512);
      for (uint32_t y = 0; y < area.height(); y += step) {
        film.readRect({area.x0, area.y0 + y, area.x1, area.y0 + y + 1}, row);
        sample.insert(sample.end(), row.begin(), row.end());
      }
      const float scale = autoExposure(sample);
      const uint32_t align = denoiser->tileAlignment();
      const uint32_t border = (std::max(32u, denoiser->tileOverlap()) + align - 1) / align * align;
      notes.push_back(fmt::format("denoising with {} (tiles of 1024 px, {} px border: OIDN's overlap {}, alignment "
                                  "{}; input scale {:.4g})",
                                  denoiser->describe(), border, denoiser->tileOverlap(), align, scale));
      // Test only (D-128): the whole image denoised in one call, the reference for the seam test.
      static const bool whole = [] {
        const char* v = std::getenv("PENUMBRA_TEST_DENOISE_WHOLE");
        if (v && *v && std::string_view(v) != "0") spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_DENOISE_WHOLE)");
        return v && *v && std::string_view(v) != "0";
      }();
      if (whole) {
        auto all = std::make_shared<std::vector<Vec3f>>();
        std::vector<Vec3f> a, nn;
        const TileRect r{area.x0, area.y0, area.x1, area.y1};
        film.readRect(r, *all);
        albedo->readRect(r, a);
        normal->readRect(r, nn);
        denoiser->run(*all, a, nn, area.width(), area.height(), scale);
        source = [all, w = area.width()](const TileRect& rows, std::vector<Vec3f>& out) {
          out.assign(rows.pixels(), Vec3f(0));
          for (uint32_t y = rows.y0; y < rows.y1; ++y)
            for (uint32_t x = rows.x0; x < rows.x1; ++x)
              out[size_t(y - rows.y0) * rows.width() + (x - rows.x0)] = (*all)[size_t(y) * w + x];
        };
      } else {
        source = [&, scale, border](const TileRect& rows, std::vector<Vec3f>& out) {
        constexpr uint32_t kTile = 1024;
        const uint32_t kBorder = border;
        out.assign(rows.pixels(), Vec3f(0));
        std::vector<Vec3f> c, a, n;
        const uint32_t y0 = rows.y0 > kBorder ? rows.y0 - kBorder : 0;
        const uint32_t y1 = std::min(rows.y1 + kBorder, area.height());
        for (uint32_t x = rows.x0; x < rows.x1; x += kTile) {
          const uint32_t tx1 = std::min(x + kTile, rows.x1);
          const uint32_t x0 = x > kBorder ? x - kBorder : 0;
          const uint32_t x1 = std::min(tx1 + kBorder, area.width());
          const TileRect r{area.x0 + x0, area.y0 + y0, area.x0 + x1, area.y0 + y1};
          film.readRect(r, c);
          albedo->readRect(r, a);
          normal->readRect(r, n);
          denoiser->run(c, a, n, x1 - x0, y1 - y0, scale);
          for (uint32_t y = rows.y0; y < rows.y1; ++y)
            for (uint32_t xx = x; xx < tx1; ++xx)
              out[size_t(y - rows.y0) * rows.width() + (xx - rows.x0)] = c[size_t(y - y0) * (x1 - x0) + (xx - x0)];
        }
      };
      }
    }
  }
  writeImage(source, area.width(), area.height(), s, metadata, notes);
}

}  // namespace pen
