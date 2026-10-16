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

std::string toHex(uint64_t v);
uint64_t fromHex(std::string_view text);  // 0 on malformed input

}  // namespace pen
