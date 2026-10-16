#pragma once

#include <string>
#include <vector>

#include "core/film/film.h"
#include "core/image/image_io.h"
#include "core/settings/render_settings.h"

namespace pen {

// Exposure, display transform and encoding from the film into the output file (whole image in M1; streamed
// tile by tile from M2). `notes` receives info lines worth logging.
void writeOutput(const Film& film, const RenderSettings& settings, const ImageMetadata& metadata,
                 std::vector<std::string>& notes);

// Display encoding for 8/16-bit formats until M11 (DECISIONS D-019): exposure, then sRGB curve with clipping.
float displayEncode(float linear, float exposureScale);

}  // namespace pen
