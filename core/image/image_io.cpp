#include "core/image/image_io.h"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <fstream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include "core/system/paths.h"

namespace pen {

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ImageIoError(fmt::format("cannot open image '{}'", pathToUtf8(path)));
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::string lowerExt(const std::filesystem::path& p) {
  std::string e = pathToUtf8(p.extension());
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// Reads R, G, B, A of an EXR (or of a crop of it) as float. Single-channel Y images are expanded to grey.
// Only the rows of the crop are decoded, so a tile of a gigapixel EXR can be read with little memory.
void readExr(const std::filesystem::path& path, const std::optional<ImageCrop>& crop, uint32_t& width,
             uint32_t& height, std::vector<float>& rgba) {
  try {
    Imf::InputFile file(pathToUtf8(path).c_str());
    const Imath::Box2i dw = file.header().dataWindow();
    const int fullW = dw.max.x - dw.min.x + 1, fullH = dw.max.y - dw.min.y + 1;
    int x0 = 0, y0 = 0, x1 = fullW, y1 = fullH;
    if (crop) {
      x0 = static_cast<int>(crop->x0);
      y0 = static_cast<int>(crop->y0);
      x1 = static_cast<int>(crop->x1);
      y1 = static_cast<int>(crop->y1);
      if (x1 > fullW || y1 > fullH || x0 >= x1 || y0 >= y1)
        throw ImageIoError(fmt::format("crop {},{},{},{} is outside the {}x{} image", x0, y0, x1, y1, fullW, fullH));
    }
    // Decode whole rows [y0, y1) (OpenEXR needs full-width slices), then keep columns [x0, x1).
    const int rows = y1 - y0;
    std::vector<float> band(size_t(fullW) * rows * 4, 0.0f);
    const bool hasRgb = file.header().channels().findChannel("R") != nullptr;
    const bool hasY = file.header().channels().findChannel("Y") != nullptr;
    Imf::FrameBuffer fb;
    const size_t xs = 4 * sizeof(float), ys = xs * size_t(fullW);
    char* base = reinterpret_cast<char*>(band.data()) - static_cast<ptrdiff_t>(dw.min.x) * xs -
                 static_cast<ptrdiff_t>(dw.min.y + y0) * ys;
    if (hasRgb || !hasY) {
      fb.insert("R", Imf::Slice(Imf::FLOAT, base, xs, ys, 1, 1, 0.0));
      fb.insert("G", Imf::Slice(Imf::FLOAT, base + sizeof(float), xs, ys, 1, 1, 0.0));
      fb.insert("B", Imf::Slice(Imf::FLOAT, base + 2 * sizeof(float), xs, ys, 1, 1, 0.0));
    } else {
      fb.insert("Y", Imf::Slice(Imf::FLOAT, base, xs, ys, 1, 1, 0.0));
    }
    fb.insert("A", Imf::Slice(Imf::FLOAT, base + 3 * sizeof(float), xs, ys, 1, 1, 1.0));
    file.setFrameBuffer(fb);
    file.readPixels(dw.min.y + y0, dw.min.y + y1 - 1);
    width = static_cast<uint32_t>(x1 - x0);
    height = static_cast<uint32_t>(rows);
    rgba.resize(size_t(width) * height * 4);
    for (int y = 0; y < rows; ++y) {
      const float* src = band.data() + (size_t(y) * fullW + x0) * 4;
      std::copy(src, src + size_t(width) * 4, rgba.data() + size_t(y) * width * 4);
    }
    if (!hasRgb && hasY) {
      for (size_t i = 0; i < size_t(width) * height; ++i) rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4];
    }
  } catch (const ImageIoError&) {
    throw;
  } catch (const std::exception& e) {
    throw ImageIoError(fmt::format("cannot read EXR '{}': {}", pathToUtf8(path), e.what()));
  }
}

}  // namespace

Texture decodeTexture(std::span<const uint8_t> bytes, const std::string& name) {
  Texture t;
  t.name = name;
  int w = 0, h = 0, n = 0;
  const auto* data = bytes.data();
  const int len = static_cast<int>(bytes.size());
  if (stbi_is_hdr_from_memory(data, len)) {
    float* px = stbi_loadf_from_memory(data, len, &w, &h, &n, 4);
    if (!px) throw ImageIoError(fmt::format("cannot decode image '{}': {}", name, stbi_failure_reason()));
    t.is_float = true;
    t.hdr.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
  } else {
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &n, 4);
    if (!px) throw ImageIoError(fmt::format("cannot decode image '{}': {}", name, stbi_failure_reason()));
    t.ldr.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
  }
  t.width = static_cast<uint32_t>(w);
  t.height = static_cast<uint32_t>(h);
  return t;
}

Texture loadTextureFile(const std::filesystem::path& path) {
  if (lowerExt(path) == ".exr") {
    Texture t;
    t.name = pathToUtf8(path.filename());
    t.is_float = true;
    readExr(path, std::nullopt, t.width, t.height, t.hdr);
    return t;
  }
  const std::vector<uint8_t> bytes = readFile(path);
  return decodeTexture(bytes, pathToUtf8(path.filename()));
}

ImageRGB loadImageRGB(const std::filesystem::path& path, const std::optional<ImageCrop>& crop) {
  Texture t;
  if (lowerExt(path) == ".exr") {
    t.is_float = true;
    readExr(path, crop, t.width, t.height, t.hdr);
  } else {
    t = loadTextureFile(path);
    if (crop) {
      if (crop->x1 > t.width || crop->y1 > t.height || crop->x0 >= crop->x1 || crop->y0 >= crop->y1)
        throw ImageIoError(fmt::format("crop is outside the {}x{} image", t.width, t.height));
      Texture c = t;
      c.width = crop->x1 - crop->x0;
      c.height = crop->y1 - crop->y0;
      if (t.is_float) {
        c.hdr.resize(size_t(c.width) * c.height * 4);
        for (uint32_t y = 0; y < c.height; ++y)
          std::copy_n(t.hdr.data() + (size_t(y + crop->y0) * t.width + crop->x0) * 4, size_t(c.width) * 4,
                      c.hdr.data() + size_t(y) * c.width * 4);
      } else {
        c.ldr.resize(size_t(c.width) * c.height * 4);
        for (uint32_t y = 0; y < c.height; ++y)
          std::copy_n(t.ldr.data() + (size_t(y + crop->y0) * t.width + crop->x0) * 4, size_t(c.width) * 4,
                      c.ldr.data() + size_t(y) * c.width * 4);
      }
      t = std::move(c);
    }
  }
  ImageRGB img;
  img.width = t.width;
  img.height = t.height;
  img.pixels.resize(size_t(t.width) * t.height);
  for (size_t i = 0; i < img.pixels.size(); ++i) {
    if (t.is_float) {
      img.pixels[i] = {t.hdr[i * 4], t.hdr[i * 4 + 1], t.hdr[i * 4 + 2]};
    } else {
      img.pixels[i] = {srgbToLinear(t.ldr[i * 4] / 255.0f), srgbToLinear(t.ldr[i * 4 + 1] / 255.0f),
                       srgbToLinear(t.ldr[i * 4 + 2] / 255.0f)};
    }
  }
  return img;
}

}  // namespace pen
