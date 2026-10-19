#include "net/join_code.h"

#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>

#include <fmt/format.h>
#include <fstream>
#include <stdexcept>

#include "core/system/file.h"

namespace pen::net {

namespace {
constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kLength = 16;
}  // namespace

std::string generateJoinCode() {
  if (psa_crypto_init() != PSA_SUCCESS) throw std::runtime_error("cannot initialise the random generator");
  uint8_t bytes[kLength];
  if (psa_generate_random(bytes, sizeof(bytes)) != PSA_SUCCESS)
    throw std::runtime_error("cannot generate a random join code");
  std::string code;
  for (uint8_t b : bytes) code += kAlphabet[b & 31];  // 5 random bits per character
  return code;
}

std::optional<std::string> normalizeJoinCode(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c == '-' || c == ' ') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    if (kAlphabet.find(c) == std::string_view::npos) return std::nullopt;
    out += c;
  }
  if (out.size() != kLength) return std::nullopt;
  return out;
}

std::string formatJoinCode(std::string_view c) {
  if (c.size() != kLength) return std::string(c);
  return fmt::format("{}-{}-{}-{}", c.substr(0, 4), c.substr(4, 4), c.substr(8, 4), c.substr(12, 4));
}

JoinKey deriveJoinKey(std::string_view canonical) {
  JoinKey key{};
  const char info[] = "penumbra-join-v1";
  const int rc = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), nullptr, 0,
                              reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(),
                              reinterpret_cast<const unsigned char*>(info), sizeof(info) - 1, key.data(), key.size());
  if (rc != 0) throw std::runtime_error("HKDF failed");
  return key;
}

std::string loadOrCreateJoinCode(const std::filesystem::path& file, bool& created) {
  created = false;
  std::ifstream in(file);
  std::string line;
  if (in && std::getline(in, line)) {
    if (auto c = normalizeJoinCode(line)) return *c;
  }
  const std::string code = generateJoinCode();
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  writeFileAtomically(file, formatJoinCode(code) + "\n");
  created = true;
  return code;
}

}  // namespace pen::net
