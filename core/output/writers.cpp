#include "core/output/writers.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfStringAttribute.h>
#include <ImfThreading.h>
#include <ImfTiledOutputFile.h>
#include <half.h>
#include <jpeglib.h>
#include <png.h>
#include <tiffio.h>
#include <webp/encode.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <thread>
#include <vector>

#include "core/system/paths.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4611)  // setjmp: the functions using it hold no destructible objects across it
#endif

namespace pen {

namespace {

FILE* openForWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
  return _wfopen(path.wstring().c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

uint32_t quantize(float v, float maxV) {
  v = v < 0 ? 0 : (v > 1 ? 1 : v);
  return static_cast<uint32_t>(std::lround(v * maxV));
}

// ---------------------------------------------------------------- EXR (tiled 256 x 256, ZIP)
class ExrWriter final : public ImageWriter {
 public:
  ExrWriter(const std::filesystem::path& path, uint32_t bitDepth, uint32_t width, uint32_t height,
            const ImageMetadata& md)
      : path_(path), width_(width), half_(bitDepth == 16) {
    Imf::Header header(static_cast<int>(width), static_cast<int>(height));
    header.compression() = Imf::ZIP_COMPRESSION;
    header.setTileDescription(Imf::TileDescription(kBandRows, kBandRows, Imf::ONE_LEVEL));
    const Imf::PixelType type = half_ ? Imf::HALF : Imf::FLOAT;
    for (const char* c : {"R", "G", "B"}) header.channels().insert(c, Imf::Channel(type));
    for (const auto& [k, v] : md) header.insert("penumbra:" + k, Imf::StringAttribute(v));
    // OpenEXR compresses on one thread unless told otherwise; gigapixel outputs need every core.
    if (Imf::globalThreadCount() == 0)
      Imf::setGlobalThreadCount(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
    try {
      file_ = std::make_unique<Imf::TiledOutputFile>(pathToUtf8(path).c_str(), header);
    } catch (const std::exception& e) {
      throw ImageIoError(fmt::format("cannot create EXR '{}': {}", pathToUtf8(path), e.what()));
    }
  }
  bool wantsLinear() const override { return true; }
  void writeBand(uint32_t y0, uint32_t rows, std::span<const float> rgb) override {
    try {
      Imf::FrameBuffer fb;
      const size_t n = size_t(width_) * rows * 3;
      char* base;
      size_t elem;
      if (half_) {
        halfs_.resize(n);
        for (size_t i = 0; i < n; ++i) halfs_[i] = Imath::half(rgb[i]);
        base = reinterpret_cast<char*>(halfs_.data());
        elem = sizeof(Imath::half);
      } else {
        base = const_cast<char*>(reinterpret_cast<const char*>(rgb.data()));
        elem = sizeof(float);
      }
      const size_t xs = 3 * elem, ys = xs * width_;
      // OpenEXR addresses pixels by absolute (x, y): shift the base so row y0 lands at the band start.
      base -= ys * y0;
      const Imf::PixelType type = half_ ? Imf::HALF : Imf::FLOAT;
      fb.insert("R", Imf::Slice(type, base, xs, ys));
      fb.insert("G", Imf::Slice(type, base + elem, xs, ys));
      fb.insert("B", Imf::Slice(type, base + 2 * elem, xs, ys));
      file_->setFrameBuffer(fb);
      const int ty = static_cast<int>(y0 / kBandRows);
      file_->writeTiles(0, file_->numXTiles() - 1, ty, ty);
      (void)rows;
    } catch (const std::exception& e) {
      throw ImageIoError(fmt::format("cannot write EXR '{}': {}", pathToUtf8(path_), e.what()));
    }
  }
  void finish() override { file_.reset(); }

 private:
  std::filesystem::path path_;
  uint32_t width_;
  bool half_;
  std::unique_ptr<Imf::TiledOutputFile> file_;
  std::vector<Imath::half> halfs_;
};

// ---------------------------------------------------------------- PNG (row-streamed)
struct PngState {
  png_structp png = nullptr;
  png_infop info = nullptr;
  char error[256] = "unknown libpng error";
};

void pngError(png_structp png, png_const_charp msg) {
  auto* st = static_cast<PngState*>(png_get_error_ptr(png));
  std::snprintf(st->error, sizeof(st->error), "%s", msg);
  png_longjmp(png, 1);
}
void pngWarning(png_structp, png_const_charp) {}

bool pngBegin(PngState* st, FILE* fp, uint32_t w, uint32_t h, int depth, png_text* texts, int nTexts) {
  st->png = png_create_write_struct(PNG_LIBPNG_VER_STRING, st, pngError, pngWarning);
  if (!st->png) return false;
  st->info = png_create_info_struct(st->png);
  if (!st->info || setjmp(png_jmpbuf(st->png))) return false;
  png_init_io(st->png, fp);
  png_set_IHDR(st->png, st->info, w, h, depth, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  if (nTexts > 0) png_set_text(st->png, st->info, texts, nTexts);
  png_write_info(st->png, st->info);
  return true;
}
bool pngRow(PngState* st, const uint8_t* row) {
  if (setjmp(png_jmpbuf(st->png))) return false;
  png_write_row(st->png, row);
  return true;
}
bool pngEnd(PngState* st) {
  if (setjmp(png_jmpbuf(st->png))) return false;
  png_write_end(st->png, nullptr);
  return true;
}

class PngWriter final : public ImageWriter {
 public:
  PngWriter(const std::filesystem::path& path, uint32_t bitDepth, uint32_t width, uint32_t height,
            const ImageMetadata& md)
      : path_(path), width_(width), depth_(bitDepth) {
    fp_ = openForWrite(path);
    if (!fp_) throw ImageIoError(fmt::format("cannot create '{}'", pathToUtf8(path)));
    for (const auto& [k, v] : md) {
      keys_.push_back(k.substr(0, 79));
      values_.push_back(v);
    }
    std::vector<png_text> texts(keys_.size());
    for (size_t i = 0; i < keys_.size(); ++i) {
      texts[i] = {};
      texts[i].compression = PNG_TEXT_COMPRESSION_NONE;
      texts[i].key = keys_[i].data();
      texts[i].text = values_[i].data();
      texts[i].text_length = values_[i].size();
    }
    if (!pngBegin(&st_, fp_, width, height, static_cast<int>(bitDepth), texts.data(), static_cast<int>(texts.size())))
      fail();
    row_.resize(size_t(width) * 3 * (bitDepth / 8));
  }
  ~PngWriter() override {
    if (st_.png) png_destroy_write_struct(&st_.png, &st_.info);
    if (fp_) std::fclose(fp_);
  }
  bool wantsLinear() const override { return false; }
  void writeBand(uint32_t, uint32_t rows, std::span<const float> rgb) override {
    const float maxV = depth_ == 16 ? 65535.0f : 255.0f;
    for (uint32_t r = 0; r < rows; ++r) {
      const float* src = rgb.data() + size_t(r) * width_ * 3;
      for (uint32_t i = 0; i < width_ * 3; ++i) {
        const uint32_t q = quantize(src[i], maxV);
        if (depth_ == 16) {
          row_[i * 2] = static_cast<uint8_t>(q >> 8);  // big-endian
          row_[i * 2 + 1] = static_cast<uint8_t>(q & 0xff);
        } else {
          row_[i] = static_cast<uint8_t>(q);
        }
      }
      if (!pngRow(&st_, row_.data())) fail();
    }
  }
  void finish() override {
    if (!pngEnd(&st_)) fail();
    png_destroy_write_struct(&st_.png, &st_.info);
    const bool ok = std::fclose(fp_) == 0;
    fp_ = nullptr;
    if (!ok) throw ImageIoError(fmt::format("cannot write PNG '{}' (disk full?)", pathToUtf8(path_)));
  }

 private:
  [[noreturn]] void fail() { throw ImageIoError(fmt::format("cannot write PNG '{}': {}", pathToUtf8(path_), st_.error)); }
  std::filesystem::path path_;
  uint32_t width_, depth_;
  FILE* fp_ = nullptr;
  PngState st_;
  std::vector<std::string> keys_, values_;
  std::vector<uint8_t> row_;
};

// ---------------------------------------------------------------- JPEG (libjpeg-turbo, scanlines)
struct JpegState {
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr err{};
  std::jmp_buf jump;
  char error[JMSG_LENGTH_MAX] = "unknown libjpeg error";
};

void jpegErrorExit(j_common_ptr c) {
  auto* st = reinterpret_cast<JpegState*>(c);  // cinfo is the first member
  (*c->err->format_message)(c, st->error);
  std::longjmp(st->jump, 1);
}

bool jpegBegin(JpegState* st, FILE* fp, uint32_t w, uint32_t h, const char* comment) {
  st->cinfo.err = jpeg_std_error(&st->err);
  st->err.error_exit = jpegErrorExit;
  if (setjmp(st->jump)) return false;
  jpeg_create_compress(&st->cinfo);
  jpeg_stdio_dest(&st->cinfo, fp);
  st->cinfo.image_width = w;
  st->cinfo.image_height = h;
  st->cinfo.input_components = 3;
  st->cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&st->cinfo);
  jpeg_set_quality(&st->cinfo, 95, TRUE);
  jpeg_start_compress(&st->cinfo, TRUE);
  jpeg_write_marker(&st->cinfo, JPEG_COM, reinterpret_cast<const JOCTET*>(comment),
                    static_cast<unsigned>(std::strlen(comment)));
  return true;
}
bool jpegRow(JpegState* st, uint8_t* row) {
  if (setjmp(st->jump)) return false;
  JSAMPROW r = row;
  jpeg_write_scanlines(&st->cinfo, &r, 1);
  return true;
}
bool jpegEnd(JpegState* st) {
  if (setjmp(st->jump)) return false;
  jpeg_finish_compress(&st->cinfo);
  return true;
}

class JpegWriter final : public ImageWriter {
 public:
  JpegWriter(const std::filesystem::path& path, uint32_t width, uint32_t height, const ImageMetadata& md)
      : path_(path), width_(width) {
    fp_ = openForWrite(path);
    if (!fp_) throw ImageIoError(fmt::format("cannot create '{}'", pathToUtf8(path)));
    for (const auto& [k, v] : md) comment_ += fmt::format("{}: {}\n", k, v);
    comment_.resize(std::min<size_t>(comment_.size(), 60000));  // JPEG markers hold < 64 KB
    st_ = std::make_unique<JpegState>();
    if (!jpegBegin(st_.get(), fp_, width, height, comment_.c_str())) fail();
    row_.resize(size_t(width) * 3);
  }
  ~JpegWriter() override {
    if (st_) jpeg_destroy_compress(&st_->cinfo);
    if (fp_) std::fclose(fp_);
  }
  bool wantsLinear() const override { return false; }
  void writeBand(uint32_t, uint32_t rows, std::span<const float> rgb) override {
    for (uint32_t r = 0; r < rows; ++r) {
      const float* src = rgb.data() + size_t(r) * width_ * 3;
      for (uint32_t i = 0; i < width_ * 3; ++i) row_[i] = static_cast<uint8_t>(quantize(src[i], 255.0f));
      if (!jpegRow(st_.get(), row_.data())) fail();
    }
  }
  void finish() override {
    if (!jpegEnd(st_.get())) fail();
    jpeg_destroy_compress(&st_->cinfo);
    st_.reset();
    const bool ok = std::fclose(fp_) == 0;
    fp_ = nullptr;
    if (!ok) throw ImageIoError(fmt::format("cannot write JPEG '{}' (disk full?)", pathToUtf8(path_)));
  }

 private:
  [[noreturn]] void fail() { throw ImageIoError(fmt::format("cannot write JPEG '{}': {}", pathToUtf8(path_), st_->error)); }
  std::filesystem::path path_;
  uint32_t width_;
  FILE* fp_ = nullptr;
  std::unique_ptr<JpegState> st_;
  std::string comment_;
  std::vector<uint8_t> row_;
};

// ---------------------------------------------------------------- TIFF (libtiff, deflate, BigTIFF > 4 GB)
thread_local std::string g_tiffError;
void tiffErrorHandler(const char* module, const char* fmtStr, va_list ap) {
  char buf[512];
  std::vsnprintf(buf, sizeof(buf), fmtStr, ap);
  g_tiffError = std::string(module ? module : "libtiff") + ": " + buf;
}

class TiffWriter final : public ImageWriter {
 public:
  TiffWriter(const std::filesystem::path& path, uint32_t bitDepth, uint32_t width, uint32_t height,
             const ImageMetadata& md)
      : path_(path), width_(width), depth_(bitDepth) {
    TIFFSetErrorHandler(tiffErrorHandler);
    TIFFSetWarningHandler(nullptr);
    const bool big = estimateOutputBytes(ImageFormat::Tiff, bitDepth, width, height) > (uint64_t(4) << 30) - (64u << 20);
#if defined(_WIN32)
    tif_ = TIFFOpenW(path.wstring().c_str(), big ? "w8" : "w");
#else
    tif_ = TIFFOpen(path.c_str(), big ? "w8" : "w");
#endif
    if (!tif_) throw ImageIoError(fmt::format("cannot create TIFF '{}': {}", pathToUtf8(path), g_tiffError));
    TIFFSetField(tif_, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif_, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif_, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif_, TIFFTAG_BITSPERSAMPLE, bitDepth);
    TIFFSetField(tif_, TIFFTAG_SAMPLEFORMAT, bitDepth == 32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
    TIFFSetField(tif_, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif_, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif_, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif_, TIFFTAG_PREDICTOR, bitDepth == 32 ? PREDICTOR_FLOATINGPOINT : PREDICTOR_HORIZONTAL);
    TIFFSetField(tif_, TIFFTAG_ROWSPERSTRIP, 16);
    std::string desc;
    for (const auto& [k, v] : md) desc += fmt::format("{}: {}\n", k, v);
    if (auto it = md.find("Software"); it != md.end()) TIFFSetField(tif_, TIFFTAG_SOFTWARE, it->second.c_str());
    TIFFSetField(tif_, TIFFTAG_IMAGEDESCRIPTION, desc.c_str());
    row_.resize(size_t(width) * 3 * (bitDepth / 8));
  }
  ~TiffWriter() override {
    if (tif_) TIFFClose(tif_);
  }
  bool wantsLinear() const override { return depth_ == 32; }
  void writeBand(uint32_t y0, uint32_t rows, std::span<const float> rgb) override {
    for (uint32_t r = 0; r < rows; ++r) {
      const float* src = rgb.data() + size_t(r) * width_ * 3;
      if (depth_ == 32) {
        std::memcpy(row_.data(), src, size_t(width_) * 3 * sizeof(float));
      } else if (depth_ == 16) {
        auto* d = reinterpret_cast<uint16_t*>(row_.data());
        for (uint32_t i = 0; i < width_ * 3; ++i) d[i] = static_cast<uint16_t>(quantize(src[i], 65535.0f));
      } else {
        for (uint32_t i = 0; i < width_ * 3; ++i) row_[i] = static_cast<uint8_t>(quantize(src[i], 255.0f));
      }
      if (TIFFWriteScanline(tif_, row_.data(), y0 + r, 0) < 0)
        throw ImageIoError(fmt::format("cannot write TIFF '{}': {}", pathToUtf8(path_), g_tiffError));
    }
  }
  void finish() override {
    const bool ok = TIFFFlush(tif_) == 1;
    TIFFClose(tif_);
    tif_ = nullptr;
    if (!ok) throw ImageIoError(fmt::format("cannot write TIFF '{}': {}", pathToUtf8(path_), g_tiffError));
  }

 private:
  std::filesystem::path path_;
  uint32_t width_, depth_;
  TIFF* tif_ = nullptr;
  std::vector<uint8_t> row_;
};

// ---------------------------------------------------------------- WebP (whole image, <= 16383 px per side)
class WebpWriter final : public ImageWriter {
 public:
  WebpWriter(const std::filesystem::path& path, uint32_t width, uint32_t height)
      : path_(path), width_(width), height_(height), pixels_(size_t(width) * height * 3) {}
  bool wantsLinear() const override { return false; }
  void writeBand(uint32_t y0, uint32_t rows, std::span<const float> rgb) override {
    uint8_t* dst = pixels_.data() + size_t(y0) * width_ * 3;
    for (size_t i = 0; i < size_t(rows) * width_ * 3; ++i) dst[i] = static_cast<uint8_t>(quantize(rgb[i], 255.0f));
  }
  void finish() override {
    uint8_t* out = nullptr;
    const size_t size = WebPEncodeRGB(pixels_.data(), static_cast<int>(width_), static_cast<int>(height_),
                                      static_cast<int>(width_ * 3), 95.0f, &out);
    if (size == 0) throw ImageIoError(fmt::format("cannot encode WebP '{}'", pathToUtf8(path_)));
    FILE* fp = openForWrite(path_);
    const bool ok = fp && std::fwrite(out, 1, size, fp) == size && std::fclose(fp) == 0;
    WebPFree(out);
    if (!ok) throw ImageIoError(fmt::format("cannot write WebP '{}' (disk full?)", pathToUtf8(path_)));
  }

 private:
  std::filesystem::path path_;
  uint32_t width_, height_;
  std::vector<uint8_t> pixels_;
};

}  // namespace

uint64_t estimateOutputBytes(ImageFormat format, uint32_t bitDepth, uint32_t width, uint32_t height) {
  const uint64_t px = uint64_t(width) * height;
  switch (format) {
    case ImageFormat::Jpeg:
    case ImageFormat::Webp: return px * 3;
    default: return px * 3 * (bitDepth / 8) + (1 << 20);
  }
}

std::unique_ptr<ImageWriter> makeImageWriter(const std::filesystem::path& path, ImageFormat format, uint32_t bitDepth,
                                             uint32_t width, uint32_t height, const ImageMetadata& metadata) {
  switch (format) {
    case ImageFormat::Exr: return std::make_unique<ExrWriter>(path, bitDepth, width, height, metadata);
    case ImageFormat::Png: return std::make_unique<PngWriter>(path, bitDepth, width, height, metadata);
    case ImageFormat::Jpeg: return std::make_unique<JpegWriter>(path, width, height, metadata);
    case ImageFormat::Tiff: return std::make_unique<TiffWriter>(path, bitDepth, width, height, metadata);
    case ImageFormat::Webp: return std::make_unique<WebpWriter>(path, width, height);
  }
  throw ImageIoError("unknown output format");
}

}  // namespace pen
