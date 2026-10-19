#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace pen {

// BLAKE3, truncated to 64 bits for checksums and fingerprints stored in films and session files.
uint64_t hash64(std::span<const uint8_t> data);
uint64_t hash64(std::string_view text);
// Hash of a file's bytes (streamed); 0 if the file cannot be read.
uint64_t hashFile(const std::filesystem::path& path);

// Streaming BLAKE3-64: the same value as hash64() of all the bytes given to update().
class Hasher {
 public:
  Hasher();
  void update(std::span<const uint8_t> data);
  uint64_t finish() const;

 private:
  alignas(8) unsigned char state_[1920];  // blake3_hasher, kept opaque so blake3.h stays private
};

std::string toHex(uint64_t v);
uint64_t fromHex(std::string_view text);  // 0 on malformed input

}  // namespace pen
