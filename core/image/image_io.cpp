#include "core/image/image_io.h"

#include <ImathBox.h>
#include <half.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
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

// Reads R, G, B, A from an EXR as float. Single-channel Y images are expanded to grey.
void readExr(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<float>& rgba) {
  try {
    Imf::InputFile file(pathToUtf8(path).c_str());
    const Imath::Box2i dw = file.header().dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    rgba.assign(size_t(w) * h * 4, 0.0f);
    const bool hasRgb = file.header().channels().findChannel("R") != nullptr;
    const bool hasY = file.header().channels().findChannel("Y") != nullptr;
    Imf::FrameBuffer fb;
    char* base = reinterpret_cast<char*>(rgba.data()) -
                 (static_cast<ptrdiff_t>(dw.min.x) + static_cast<ptrdiff_t>(dw.min.y) * w) * 4 * sizeof(float);
    const size_t xs = 4 * sizeof(float), ys = xs * size_t(w);
    if (hasRgb || !hasY) {
      fb.insert("R", Imf::Slice(Imf::FLOAT, base, xs, ys, 1, 1, 0.0));
      fb.insert("G", Imf::Slice(Imf::FLOAT, base + sizeof(float), xs, ys, 1, 1, 0.0));
      fb.insert("B", Imf::Slice(Imf::FLOAT, base + 2 * sizeof(float), xs, ys, 1, 1, 0.0));
    } else {
      fb.insert("Y", Imf::Slice(Imf::FLOAT, base, xs, ys, 1, 1, 0.0));
    }
    fb.insert("A", Imf::Slice(Imf::FLOAT, base + 3 * sizeof(float), xs, ys, 1, 1, 1.0));
    file.setFrameBuffer(fb);
    file.readPixels(dw.min.y, dw.max.y);
    if (!hasRgb && hasY) {
      for (size_t i = 0; i < size_t(w) * h; ++i) rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4];
    }
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
    readExr(path, t.width, t.height, t.hdr);
    return t;
  }
  const std::vector<uint8_t> bytes = readFile(path);
  return decodeTexture(bytes, pathToUtf8(path.filename()));
}

ImageRGB loadImageRGB(const std::filesystem::path& path) {
  const Texture t = loadTextureFile(path);
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

namespace {

struct PngError {
  char message[256] = "unknown libpng error";
};

void pngErrorFn(png_structp png, png_const_charp msg) {
  auto* err = static_cast<PngError*>(png_get_error_ptr(png));
  std::snprintf(err->message, sizeof(err->message), "%s", msg);
  png_longjmp(png, 1);
}
void pngWarnFn(png_structp, png_const_charp) {}

// All libpng calls live here, with no C++ objects whose lifetime crosses setjmp.
#if defined(_MSC_VER)
#pragma warning(disable : 4611)  // setjmp and C++ destruction: this function has no destructible objects
#endif
bool writePngRaw(FILE* fp, uint32_t width, uint32_t height, const float* rgb, uint32_t bitDepth, png_text* texts,
                 int textCount, uint8_t* row, PngError* err) {
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, err, pngErrorFn, pngWarnFn);
  if (!png) return false;
  png_infop info = png_create_info_struct(png);
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return false;
  }
  png_init_io(png, fp);
  png_set_IHDR(png, info, width, height, static_cast<int>(bitDepth), PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  if (textCount > 0) png_set_text(png, info, texts, textCount);
  png_write_info(png, info);
  const float maxV = bitDepth == 16 ? 65535.0f : 255.0f;
  for (uint32_t y = 0; y < height; ++y) {
    const float* src = rgb + size_t(y) * width * 3;
    for (uint32_t i = 0; i < width * 3; ++i) {
      const float v = src[i] < 0 ? 0 : (src[i] > 1 ? 1 : src[i]);
      const uint32_t q = static_cast<uint32_t>(std::lround(v * maxV));
      if (bitDepth == 16) {
        row[i * 2] = static_cast<uint8_t>(q >> 8);  // PNG is big-endian
        row[i * 2 + 1] = static_cast<uint8_t>(q & 0xff);
      } else {
        row[i] = static_cast<uint8_t>(q);
      }
    }
    png_write_row(png, row);
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return true;
}

FILE* openForWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
  return _wfopen(path.wstring().c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

}  // namespace

void writePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb,
              uint32_t bitDepth, const ImageMetadata& metadata) {
  if (bitDepth != 8 && bitDepth != 16) throw ImageIoError("PNG supports 8 or 16 bits per channel");
  FILE* fp = openForWrite(path);
  if (!fp) throw ImageIoError(fmt::format("cannot create '{}'", pathToUtf8(path)));
  std::vector<std::string> keys, values;
  for (const auto& [k, v] : metadata) {
    keys.push_back(k.substr(0, 79));  // PNG keyword limit
    values.push_back(v);
  }
  std::vector<png_text> texts(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    texts[i] = {};
    texts[i].compression = PNG_TEXT_COMPRESSION_NONE;
    texts[i].key = keys[i].data();
    texts[i].text = values[i].data();
    texts[i].text_length = values[i].size();
  }
  std::vector<uint8_t> row(size_t(width) * 3 * (bitDepth / 8));
  PngError err;
  const bool ok = writePngRaw(fp, width, height, rgb.data(), bitDepth, texts.data(), static_cast<int>(texts.size()),
                              row.data(), &err);
  const bool closed = std::fclose(fp) == 0;
  if (!ok) throw ImageIoError(fmt::format("cannot write PNG '{}': {}", pathToUtf8(path), err.message));
  if (!closed) throw ImageIoError(fmt::format("cannot write PNG '{}' (disk full?)", pathToUtf8(path)));
}

void writeExr(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb,
              uint32_t bitDepth, const ImageMetadata& metadata) {
  if (bitDepth != 16 && bitDepth != 32) throw ImageIoError("EXR supports 16 (half) or 32 (float) bits per channel");
  try {
    Imf::Header header(static_cast<int>(width), static_cast<int>(height));
    header.compression() = Imf::ZIP_COMPRESSION;
    const Imf::PixelType type = bitDepth == 16 ? Imf::HALF : Imf::FLOAT;
    header.channels().insert("R", Imf::Channel(type));
    header.channels().insert("G", Imf::Channel(type));
    header.channels().insert("B", Imf::Channel(type));
    for (const auto& [k, v] : metadata) header.insert("penumbra:" + k, Imf::StringAttribute(v));
    Imf::OutputFile file(pathToUtf8(path).c_str(), header);
    Imf::FrameBuffer fb;
    // OpenEXR does not convert float to half on write, so half output gets its own buffer.
    std::vector<Imath::half> halfs;
    char* base = const_cast<char*>(reinterpret_cast<const char*>(rgb.data()));
    size_t elem = sizeof(float);
    if (type == Imf::HALF) {
      halfs.resize(rgb.size());
      for (size_t i = 0; i < rgb.size(); ++i) halfs[i] = Imath::half(rgb[i]);
      base = reinterpret_cast<char*>(halfs.data());
      elem = sizeof(Imath::half);
    }
    const size_t xs = 3 * elem, ys = xs * width;
    fb.insert("R", Imf::Slice(type, base, xs, ys));
    fb.insert("G", Imf::Slice(type, base + elem, xs, ys));
    fb.insert("B", Imf::Slice(type, base + 2 * elem, xs, ys));
    file.setFrameBuffer(fb);
    file.writePixels(static_cast<int>(height));
  } catch (const std::exception& e) {
    throw ImageIoError(fmt::format("cannot write EXR '{}': {}", pathToUtf8(path), e.what()));
  }
}

}  // namespace pen
