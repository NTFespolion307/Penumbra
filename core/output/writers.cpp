#include "core/output/writers.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfStringAttribute.h>
#include <ImfThreading.h>
#include <ImfTiledOutputFile.h>
#include <half.h>
#include <jpeglib.h>
#include <tiffio.h>
#include <webp/encode.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/system/paths.h"
#include "core/system/sysinfo.h"

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
      Imf::setGlobalThreadCount(static_cast<int>(usableCpuThreads()));
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

// ---------------------------------------------------------------- PNG (bands compressed in parallel)
// libpng deflates on one thread, which took minutes for a 40K image. Here each band of rows is filtered and deflated
// on its own thread into a raw deflate stream that ends on a byte boundary (Z_SYNC_FLUSH; the last band Z_FINISH).
// The streams are written in order as one zlib stream (header, the bands, the Adler-32 combined from the bands') in
// IDAT chunks, as pigz does. A band starts with an empty dictionary, which costs well under 1% of the size. Rows use
// libpng's filter choice: the filter with the smallest sum of absolute (signed) bytes.

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// One row filtered into out[0] (the filter type) and out[1..len]; `prev` is the row above (zeros for the first).
void filterRow(const uint8_t* row, const uint8_t* prev, size_t len, size_t bpp, uint8_t* out, uint8_t* scratch) {
  const auto apply = [&](uint8_t type, uint8_t* f) {
    const size_t head = std::min(bpp, len);  // no pixel to the left: a = c = 0
    switch (type) {
      case 0: std::memcpy(f, row, len); break;
      case 1:
        std::memcpy(f, row, head);
        for (size_t i = head; i < len; ++i) f[i] = static_cast<uint8_t>(row[i] - row[i - bpp]);
        break;
      case 2:
        for (size_t i = 0; i < len; ++i) f[i] = static_cast<uint8_t>(row[i] - prev[i]);
        break;
      case 3:
        for (size_t i = 0; i < head; ++i) f[i] = static_cast<uint8_t>(row[i] - prev[i] / 2);
        for (size_t i = head; i < len; ++i) f[i] = static_cast<uint8_t>(row[i] - (row[i - bpp] + prev[i]) / 2);
        break;
      default:
        for (size_t i = 0; i < head; ++i) f[i] = static_cast<uint8_t>(row[i] - prev[i]);  // Paeth(0, b, 0) = b
        for (size_t i = head; i < len; ++i) f[i] = static_cast<uint8_t>(row[i] - paeth(row[i - bpp], prev[i], prev[i - bpp]));
        break;
    }
  };
  uint64_t bestSum = UINT64_MAX;
  for (uint8_t type = 0; type < 5; ++type) {
    apply(type, scratch);
    uint64_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(scratch[i]))));
    if (sum < bestSum) {
      bestSum = sum;
      out[0] = type;
      std::memcpy(out + 1, scratch, len);
    }
  }
}

struct PngBand {
  std::vector<uint8_t> deflated;
  uLong adler = 1;
  size_t filteredBytes = 0;
};

// Filters and deflates rows (`rows` rows of `len` bytes) whose row above is `prev`.
PngBand compressPngBand(const std::vector<uint8_t>& rows, const std::vector<uint8_t>& prev, size_t len, size_t bpp,
                        bool last) {
  const size_t n = rows.size() / len;
  std::vector<uint8_t> filtered(n * (len + 1)), scratch(len);
  for (size_t r = 0; r < n; ++r)
    filterRow(rows.data() + r * len, r == 0 ? prev.data() : rows.data() + (r - 1) * len, len, bpp,
              filtered.data() + r * (len + 1), scratch.data());
  PngBand band;
  band.filteredBytes = filtered.size();
  band.adler = adler32(1, filtered.data(), static_cast<uInt>(filtered.size()));
  z_stream zs{};
  if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_FILTERED) != Z_OK) throw ImageIoError("deflateInit2 failed");
  band.deflated.resize(deflateBound(&zs, static_cast<uLong>(filtered.size())) + 64);
  zs.next_in = filtered.data();
  zs.avail_in = static_cast<uInt>(filtered.size());
  zs.next_out = band.deflated.data();
  zs.avail_out = static_cast<uInt>(band.deflated.size());
  const int rc = deflate(&zs, last ? Z_FINISH : Z_SYNC_FLUSH);
  const bool ok = last ? rc == Z_STREAM_END : (rc == Z_OK && zs.avail_in == 0 && zs.avail_out > 0);
  band.deflated.resize(band.deflated.size() - zs.avail_out);
  deflateEnd(&zs);
  if (!ok) throw ImageIoError(fmt::format("deflate failed ({})", rc));
  return band;
}

class PngWriter final : public ImageWriter {
 public:
  PngWriter(const std::filesystem::path& path, uint32_t bitDepth, uint32_t width, uint32_t height,
            const ImageMetadata& md)
      : path_(path), width_(width), height_(height), depth_(bitDepth), bpp_(3 * (bitDepth / 8)),
        len_(size_t(width) * bpp_), prev_(len_, 0) {
    fp_ = openForWrite(path);
    if (!fp_) throw ImageIoError(fmt::format("cannot create '{}'", pathToUtf8(path)));
    static const uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    put(kSignature, sizeof(kSignature));
    std::vector<uint8_t> ihdr;
    putU32(ihdr, width);
    putU32(ihdr, height);
    ihdr.insert(ihdr.end(), {static_cast<uint8_t>(bitDepth), 2 /* RGB */, 0, 0, 0 /* no interlace */});
    chunk("IHDR", ihdr.data(), ihdr.size());
    for (const auto& [k, v] : md) {  // tEXt: keyword (1-79 bytes), NUL, text
      std::vector<uint8_t> text(k.begin(), k.begin() + std::ptrdiff_t(std::min<size_t>(k.size(), 79)));
      text.push_back(0);
      text.insert(text.end(), v.begin(), v.end());
      chunk("tEXt", text.data(), text.size());
    }
    // Bands in flight: every core busy, but their rows (quantized, filtered, deflated) within about 1.5 GB.
    const size_t bandBytes = len_ * kBandRows * 3;
    maxInFlight_ = std::clamp<size_t>(size_t(1536) * 1024 * 1024 / std::max<size_t>(bandBytes, 1), 2,
                                      std::max<size_t>(usableCpuThreads(), 2));
  }
  ~PngWriter() override {
    for (auto& f : inFlight_)
      if (f.valid()) f.wait();
    if (fp_) std::fclose(fp_);
  }
  bool wantsLinear() const override { return false; }
  void writeBand(uint32_t y0, uint32_t rows, std::span<const float> rgb) override {
    const float maxV = depth_ == 16 ? 65535.0f : 255.0f;
    std::vector<uint8_t> q(len_ * rows);
    for (size_t i = 0, n = size_t(width_) * 3 * rows; i < n; ++i) {
      const uint32_t v = quantize(rgb[i], maxV);
      if (depth_ == 16) {
        q[i * 2] = static_cast<uint8_t>(v >> 8);  // big-endian
        q[i * 2 + 1] = static_cast<uint8_t>(v & 0xff);
      } else {
        q[i] = static_cast<uint8_t>(v);
      }
    }
    std::vector<uint8_t> prev = std::exchange(prev_, std::vector<uint8_t>(q.end() - std::ptrdiff_t(len_), q.end()));
    const bool last = y0 + rows >= height_;
    inFlight_.push_back(std::async(std::launch::async, [q = std::move(q), prev = std::move(prev), this, last] {
      return compressPngBand(q, prev, len_, bpp_, last);
    }));
    while (inFlight_.size() > maxInFlight_) writeOldest();
  }
  void finish() override {
    while (!inFlight_.empty()) writeOldest();
    std::vector<uint8_t> tail;
    putU32(tail, static_cast<uint32_t>(adler_));
    chunk("IDAT", tail.data(), tail.size());
    chunk("IEND", nullptr, 0);
    const bool ok = std::fclose(fp_) == 0;
    fp_ = nullptr;
    if (!ok) throw ImageIoError(fmt::format("cannot write PNG '{}' (disk full?)", pathToUtf8(path_)));
  }

 private:
  void writeOldest() {
    PngBand band;
    try {
      band = inFlight_.front().get();
    } catch (const std::exception& e) {
      inFlight_.pop_front();
      throw ImageIoError(fmt::format("cannot write PNG '{}': {}", pathToUtf8(path_), e.what()));
    }
    inFlight_.pop_front();
    if (first_) {  // zlib header: deflate, 32 KB window, default level
      static const uint8_t kZlibHeader[2] = {0x78, 0x9c};
      band.deflated.insert(band.deflated.begin(), kZlibHeader, kZlibHeader + 2);
      first_ = false;
    }
    adler_ = adler32_combine(adler_, band.adler, static_cast<z_off_t>(band.filteredBytes));
    constexpr size_t kMaxChunk = size_t(1) << 26;
    for (size_t off = 0; off < band.deflated.size(); off += kMaxChunk)
      chunk("IDAT", band.deflated.data() + off, std::min(kMaxChunk, band.deflated.size() - off));
  }
  void put(const void* data, size_t n) {
    if (n && std::fwrite(data, 1, n, fp_) != n)
      throw ImageIoError(fmt::format("cannot write PNG '{}' (disk full?)", pathToUtf8(path_)));
  }
  void chunk(const char type[4], const uint8_t* data, size_t n) {
    std::vector<uint8_t> head;
    putU32(head, static_cast<uint32_t>(n));
    head.insert(head.end(), type, type + 4);
    put(head.data(), head.size());
    put(data, n);
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    if (n) crc = crc32(crc, data, static_cast<uInt>(n));
    std::vector<uint8_t> tail;
    putU32(tail, static_cast<uint32_t>(crc));
    put(tail.data(), tail.size());
  }

  std::filesystem::path path_;
  uint32_t width_, height_, depth_;
  size_t bpp_, len_;
  std::vector<uint8_t> prev_;  // the last row written so far (zeros before the first band)
  FILE* fp_ = nullptr;
  std::deque<std::future<PngBand>> inFlight_;
  size_t maxInFlight_ = 2;
  uLong adler_ = 1;
  bool first_ = true;
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
