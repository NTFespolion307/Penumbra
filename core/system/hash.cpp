#include "core/system/hash.h"

#include <blake3.h>

#include <charconv>
#include <fmt/format.h>
#include <fstream>
#include <vector>

namespace pen {

namespace {
uint64_t finish(blake3_hasher& h) {
  uint8_t out[8];
  blake3_hasher_finalize(&h, out, sizeof(out));
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(out[i]) << (8 * i);
  return v;
}
}  // namespace

uint64_t hash64(std::span<const uint8_t> data) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data.data(), data.size());
  return finish(h);
}

uint64_t hash64(std::string_view text) {
  return hash64({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

uint64_t hashFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return 0;
  blake3_hasher h;
  blake3_hasher_init(&h);
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    blake3_hasher_update(&h, buf.data(), static_cast<size_t>(in.gcount()));
  }
  return finish(h);
}

static_assert(sizeof(blake3_hasher) <= 1920, "Hasher::state_ is too small for blake3_hasher");

Hasher::Hasher() { blake3_hasher_init(reinterpret_cast<blake3_hasher*>(state_)); }

void Hasher::update(std::span<const uint8_t> data) {
  blake3_hasher_update(reinterpret_cast<blake3_hasher*>(state_), data.data(), data.size());
}

uint64_t Hasher::finish() const {
  blake3_hasher copy = *reinterpret_cast<const blake3_hasher*>(state_);
  return pen::finish(copy);
}

std::string toHex(uint64_t v) { return fmt::format("{:016x}", v); }

uint64_t fromHex(std::string_view text) {
  uint64_t v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
  return ec == std::errc() && ptr == text.data() + text.size() ? v : 0;
}

}  // namespace pen
