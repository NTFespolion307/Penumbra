#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/settings/render_settings.h"

namespace pen {

// Thrown by every parser in this header; the message says what is wrong with the value, without naming the
// option (the caller adds that).
struct ValueError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string toLower(std::string_view s);
std::string_view trim(std::string_view s);

// Unsigned integer, digits only, within [lo, hi].
uint64_t parseUint(std::string_view text, uint64_t lo, uint64_t hi);
// Decimal number within [lo, hi].
double parseDouble(std::string_view text, double lo, double hi);
// true/false, on/off, yes/no, 1/0.
bool parseBool(std::string_view text);

// "2h30m", "10m", "90s", "1d", "45" (seconds). Returns seconds.
uint64_t parseDuration(std::string_view text);
// "6G", "512M", "1.5G", "20GB", "1024" (bytes). Binary units: K = 1024. Returns bytes.
uint64_t parseByteSize(std::string_view text);
// A preset name ("4k") or "WxH" ("3000x2000"), each side 16..163840.
Resolution parseResolution(std::string_view text);
// "x0,y0,x1,y1", half-open.
PixelRect parseRect(std::string_view text);
// "a-b", inclusive.
SampleRange parseSampleRange(std::string_view text);

std::string formatDuration(uint64_t seconds);  // "2h30m", "10m", "45s"
std::string formatBytes(uint64_t bytes);       // "20.0 GB" (binary units, shown as GB)

}  // namespace pen
