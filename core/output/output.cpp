#include "core/output/output.h"

#include <cmath>
#include <fmt/format.h>

#include "core/image/image.h"

namespace pen {

float displayEncode(float linear, float exposureScale) {
  return linearToSrgb(std::fmin(std::fmax(linear * exposureScale, 0.0f), 1.0f));
}

void writeOutput(const Film& film, const RenderSettings& s, const ImageMetadata& metadata,
                 std::vector<std::string>& notes) {
  const float exposure = std::exp2(static_cast<float>(s.exposure));
  const ImageFormat format = s.format.value_or(ImageFormat::Png);
  std::vector<float> rgb(size_t(film.width()) * film.height() * 3);
  const auto& means = film.means();

  if (format == ImageFormat::Exr) {
    for (size_t i = 0; i < means.size(); ++i) {
      rgb[i * 3] = means[i].x * exposure;
      rgb[i * 3 + 1] = means[i].y * exposure;
      rgb[i * 3 + 2] = means[i].z * exposure;
    }
    if (s.tonemap != Tonemap::None)
      notes.push_back(fmt::format("EXR output is linear; --tonemap {} applies to display formats only",
                                  toString(s.tonemap)));
    writeExr(s.output, film.width(), film.height(), rgb, s.bit_depth, metadata);
    return;
  }

  if (format != ImageFormat::Png)
    throw ImageIoError(fmt::format("{} output is built in milestone M2; use .png or .exr", toString(format)));
  if (s.tonemap != Tonemap::None)
    notes.push_back(fmt::format("tonemap {} is built in milestone M11; this render uses the sRGB curve with "
                                "clipping (--tonemap none)",
                                toString(s.tonemap)));
  for (size_t i = 0; i < means.size(); ++i) {
    rgb[i * 3] = displayEncode(means[i].x, exposure);
    rgb[i * 3 + 1] = displayEncode(means[i].y, exposure);
    rgb[i * 3 + 2] = displayEncode(means[i].z, exposure);
  }
  writePng(s.output, film.width(), film.height(), rgb, s.bit_depth, metadata);
}

}  // namespace pen
