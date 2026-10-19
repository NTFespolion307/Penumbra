#pragma once

// Join codes (docs/GUIDE.md "Join code"): 16 characters of Crockford base32 (0-9, A-Z without I, L, O, U), about
// 80 bits, shown as XXXX-XXXX-XXXX-XXXX and typed case-insensitively with dashes optional. The code is never
// sent: both sides derive the TLS pre-shared key from it.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "net/tls.h"

namespace pen::net {

// A new random code, 16 characters without dashes.
std::string generateJoinCode();
// Canonical 16 characters, or nullopt when the text is not a code. Case-insensitive; dashes and spaces are
// ignored; I and L read as 1, O as 0 (Crockford's decoding rules).
std::optional<std::string> normalizeJoinCode(std::string_view text);
// "K7QDM2XR9FHT4WNB" -> "K7QD-M2XR-9FHT-4WNB".
std::string formatJoinCode(std::string_view canonical);
// HKDF-SHA256(code, info "penumbra-join-v1") -> 32-byte TLS pre-shared key.
JoinKey deriveJoinKey(std::string_view canonical);

// The controller's persistent code in `file` (user data folder): read it, or create and save a new one.
// Throws std::runtime_error when the file cannot be written.
std::string loadOrCreateJoinCode(const std::filesystem::path& file, bool& created);

}  // namespace pen::net
