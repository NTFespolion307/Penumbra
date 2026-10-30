#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/film/film.h"
#include "core/image/image_io.h"
#include "core/render/work.h"
#include "core/settings/render_settings.h"

namespace pen {

// Denoise (--denoise, with the albedo and normal films), exposure, display transform and encoding, streamed band
// by band from the film into the output file. The output covers the whole image, or only --region when one is
// set. `notes` receives info lines to log.
void writeOutput(const Film& film, const RenderSettings& settings, const ImageMetadata& metadata,
                 std::vector<std::string>& notes, const Film* albedo = nullptr, const Film* normal = nullptr);

// Same, from any source of linear radiance rows (used by `penumbra merge`).
using RowSource = std::function<void(const TileRect& rows, std::vector<Vec3f>& out)>;
void writeImage(const RowSource& source, uint32_t width, uint32_t height, const RenderSettings& settings,
                const ImageMetadata& metadata, std::vector<std::string>& notes);

// Display encoding for 8/16-bit formats: exposure, then the tonemap (core/output/tonemap.h).
Vec3f displayEncode(Vec3f linear, float exposureScale, Tonemap tonemap);

// An AOV film (M11, D-127) as <output name>.<pass>.exr next to the output: linear float, no exposure or tonemap;
// albedo, normals in [-1, 1] (0 where nothing was hit), depth in scene units (32-bit).
void writeAov(const Film& film, Pass pass, const RenderSettings& s, const ImageMetadata& metadata,
              std::vector<std::string>& notes);
std::filesystem::path aovOutputPath(const std::filesystem::path& output, Pass pass);

// The pixel rectangle the output file covers: --region, or the whole image.
TileRect outputRect(const RenderSettings& s);

}  // namespace pen
