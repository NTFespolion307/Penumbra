#include "net/tile_codec.h"

#include <zstd.h>

#include <cstring>
#include <fmt/format.h>

#include "core/system/hash.h"
#include "net/protocol.h"

namespace pen::net {

namespace {
constexpr int kZstdLevel = 3;
static_assert(sizeof(Vec3f) == 12, "Vec3f must be three packed floats");
}  // namespace

std::vector<uint8_t> encodeTilePixels(std::span<const Vec3f> mean) {
  const size_t n = mean.size() * 3;  // floats
  const auto* src = reinterpret_cast<const uint8_t*>(mean.data());
  std::vector<uint8_t> planes(n * 4);
  for (size_t i = 0; i < n; ++i)
    for (size_t b = 0; b < 4; ++b) planes[b * n + i] = src[i * 4 + b];
  std::vector<uint8_t> out(ZSTD_compressBound(planes.size()));
  const size_t size = ZSTD_compress(out.data(), out.size(), planes.data(), planes.size(), kZstdLevel);
  if (ZSTD_isError(size)) throw ProtocolError(fmt::format("zstd: {}", ZSTD_getErrorName(size)));
  out.resize(size);
  return out;
}

std::vector<Vec3f> decodeTilePixels(std::span<const uint8_t> data, uint64_t pixels) {
  const size_t n = pixels * 3;
  const unsigned long long declared = ZSTD_getFrameContentSize(data.data(), data.size());
  if (declared != n * 4) throw ProtocolError("tile pixels have the wrong size");
  std::vector<uint8_t> planes(n * 4);
  const size_t size = ZSTD_decompress(planes.data(), planes.size(), data.data(), data.size());
  if (ZSTD_isError(size) || size != planes.size()) throw ProtocolError("tile pixels do not decompress");
  std::vector<Vec3f> mean(pixels);
  auto* dst = reinterpret_cast<uint8_t*>(mean.data());
  for (size_t i = 0; i < n; ++i)
    for (size_t b = 0; b < 4; ++b) dst[i * 4 + b] = planes[b * n + i];
  return mean;
}

std::vector<uint8_t> compress(std::span<const uint8_t> data) {
  std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
  const size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), kZstdLevel);
  if (ZSTD_isError(size)) throw ProtocolError(fmt::format("zstd: {}", ZSTD_getErrorName(size)));
  out.resize(size);
  return out;
}

std::vector<uint8_t> decompress(std::span<const uint8_t> data, size_t size) {
  if (ZSTD_getFrameContentSize(data.data(), data.size()) != size) throw ProtocolError("compressed data has the wrong size");
  std::vector<uint8_t> out(size);
  const size_t n = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
  if (ZSTD_isError(n) || n != size) throw ProtocolError("compressed data does not decompress");
  return out;
}

uint64_t tileChecksum(std::span<const Vec3f> mean) {
  return hash64(std::span(reinterpret_cast<const uint8_t*>(mean.data()), mean.size() * sizeof(Vec3f)));
}

}  // namespace pen::net
