#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pen {

// Wire protocol version. Controller and workers must match exactly (see docs/PROTOCOL.md from M4).
inline constexpr uint16_t kProtocolVersion = 2;

std::string_view appVersion();  // e.g. "0.1.0" (generated)
std::string_view gitHash();     // e.g. "3f2a9c1b7d04", "...-dirty" or "unknown" (generated)

// One line for --version and logs: "Penumbra 0.1.0 (git 3f2a9c1b7d04, Release, MSVC 19.51, Windows x64)".
std::string versionString();

}  // namespace pen
