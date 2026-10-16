#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>

#include "core/image/image.h"

namespace pen {

struct ImageIoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Decodes PNG, JPEG, BMP, TGA or Radiance HDR from memory into a texture (wrap/filter left at defaults).
Texture decodeTexture(std::span<const uint8_t> bytes, const std::string& name);
// Loads PNG, JPEG, HDR or EXR from a file into a texture.
Texture loadTextureFile(const std::filesystem::path& path);

// Loads an image file (EXR, HDR, PNG, JPEG) as linear RGB. 8-bit formats are decoded from sRGB.
ImageRGB loadImageRGB(const std::filesystem::path& path);

using ImageMetadata = std::map<std::string, std::string>;

// 8- or 16-bit PNG from already display-encoded values in [0,1], streamed row by row.
void writePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb,
              uint32_t bitDepth, const ImageMetadata& metadata);

// Linear RGB EXR, half (16) or float (32), ZIP compression.
void writeExr(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb,
              uint32_t bitDepth, const ImageMetadata& metadata);

}  // namespace pen
