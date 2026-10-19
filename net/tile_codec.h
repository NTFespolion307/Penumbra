#pragma once

// Tile pixels on the wire (DECISIONS D-053): the RGB means as float32, split into 4 byte planes (all lowest
// bytes, then the next...), then zstd level 3. Lossless, so a tile rendered on any host merges to the same
// bits as a local one; the byte planes let zstd find the repetition in exponents and high mantissa bytes.

#include <cstdint>
#include <span>
#include <vector>

#include "core/math/vec.h"

namespace pen::net {

std::vector<uint8_t> encodeTilePixels(std::span<const Vec3f> mean);
// Throws ProtocolError when the data does not decode to exactly `pixels` RGB values.
std::vector<Vec3f> decodeTilePixels(std::span<const uint8_t> data, uint64_t pixels);
// BLAKE3-64 of the float32 RGB values as stored in memory (little-endian on every supported host).
uint64_t tileChecksum(std::span<const Vec3f> mean);

}  // namespace pen::net
