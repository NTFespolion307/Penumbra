#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "core/image/image.h"

namespace pen {

struct ImageIoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Half-open pixel rectangle of an image file to load.
struct ImageCrop {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Decodes PNG, JPEG, BMP, TGA or Radiance HDR from memory into a texture (wrap/filter left at defaults).
Texture decodeTexture(std::span<const uint8_t> bytes, const std::string& name);
// Loads PNG, JPEG, HDR or EXR from a file into a texture.
Texture loadTextureFile(const std::filesystem::path& path);

// Loads an image file (EXR, HDR, PNG, JPEG) as linear RGB; 8-bit formats are decoded from sRGB. With a crop,
// EXR files decode only the rows needed, so a tile of a gigapixel EXR loads quickly.
ImageRGB loadImageRGB(const std::filesystem::path& path, const std::optional<ImageCrop>& crop = std::nullopt);

using ImageMetadata = std::map<std::string, std::string>;

}  // namespace pen
