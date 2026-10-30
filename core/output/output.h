#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/film/film.h"
#include "core/image/image_io.h"
#include "core/render/work.h"
#include "core/settings/render_settings.h"

namespace pen {

// Exposure, display transform and encoding, streamed band by band from the film into the output file.
// The output covers the whole image, or only --region when one is set. `notes` receives info lines to log.
void writeOutput(const Film& film, const RenderSettings& settings, const ImageMetadata& metadata,
                 std::vector<std::string>& notes);

// Same, from any source of linear radiance rows (used by `penumbra merge`).
using RowSource = std::function<void(const TileRect& rows, std::vector<Vec3f>& out)>;
void writeImage(const RowSource& source, uint32_t width, uint32_t height, const RenderSettings& settings,
                const ImageMetadata& metadata, std::vector<std::string>& notes);

// Display encoding for 8/16-bit formats: exposure, then the tonemap (core/output/tonemap.h).
Vec3f displayEncode(Vec3f linear, float exposureScale, Tonemap tonemap);

// The pixel rectangle the output file covers: --region, or the whole image.
TileRect outputRect(const RenderSettings& s);

}  // namespace pen
