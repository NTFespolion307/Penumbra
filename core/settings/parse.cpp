#include "core/settings/parse.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <vector>

#include "core/settings/presets.h"

namespace pen {

std::string toLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

namespace {

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (;;) {
    const size_t pos = s.find(sep, start);
    parts.push_back(trim(s.substr(start, pos - start)));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return parts;
}

// Parses a leading unsigned decimal; returns characters consumed (0 if none).
size_t leadingUint(std::string_view s, uint64_t& value) {
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec == std::errc::result_out_of_range) throw ValueError(fmt::format("'{}' is too large", s));
  if (ec != std::errc()) return 0;
  return static_cast<size_t>(ptr - s.data());
}

}  // namespace

uint64_t parseUint(std::string_view text, uint64_t lo, uint64_t hi) {
  text = trim(text);
  uint64_t v = 0;
  if (text.empty() || leadingUint(text, v) != text.size())
    throw ValueError(fmt::format("'{}' is not a whole number", text));
  if (v < lo || v > hi) throw ValueError(fmt::format("{} is out of range ({} to {})", v, lo, hi));
  return v;
}

double parseDouble(std::string_view text, double lo, double hi) {
  text = trim(text);
  double v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (text.empty() || ec != std::errc() || ptr != text.data() + text.size() || !std::isfinite(v))
    throw ValueError(fmt::format("'{}' is not a number", text));
  if (v < lo || v > hi) throw ValueError(fmt::format("{} is out of range ({} to {})", v, lo, hi));
  return v;
}

bool parseBool(std::string_view text) {
  const std::string t = toLower(trim(text));
  if (t == "true" || t == "on" || t == "yes" || t == "1") return true;
  if (t == "false" || t == "off" || t == "no" || t == "0") return false;
  throw ValueError(fmt::format("'{}' is not true or false", text));
}

uint64_t parseDuration(std::string_view text) {
  const std::string t = toLower(trim(text));
  if (t.empty()) throw ValueError("empty duration");
  std::string_view s = t;
  uint64_t total = 0;
  bool any = false;
  while (!s.empty()) {
    uint64_t n = 0;
    const size_t used = leadingUint(s, n);
    if (used == 0) throw ValueError(fmt::format("'{}' is not a duration like 2h30m, 10m or 90s", text));
    s.remove_prefix(used);
    uint64_t unit = 1;
    if (!s.empty()) {
      switch (s.front()) {
        case 'd': unit = 86400; break;
        case 'h': unit = 3600; break;
        case 'm': unit = 60; break;
        case 's': unit = 1; break;
        default: throw ValueError(fmt::format("'{}' has an unknown unit (use d, h, m, s)", text));
      }
      s.remove_prefix(1);
    } else if (any) {
      throw ValueError(fmt::format("'{}' is missing a unit after the last number", text));
    }
    if (n > std::numeric_limits<uint64_t>::max() / unit) throw ValueError(fmt::format("'{}' is too long", text));
    total += n * unit;
    any = true;
  }
  return total;
}

uint64_t parseByteSize(std::string_view text) {
  const std::string t = toLower(trim(text));
  size_t numEnd = 0;
  while (numEnd < t.size() && (std::isdigit(static_cast<unsigned char>(t[numEnd])) || t[numEnd] == '.')) ++numEnd;
  if (numEnd == 0) throw ValueError(fmt::format("'{}' is not a size like 6G or 512M", text));
  double number = 0;
  const auto [ptr, ec] = std::from_chars(t.data(), t.data() + numEnd, number);
  if (ec != std::errc() || ptr != t.data() + numEnd) throw ValueError(fmt::format("'{}' is not a size", text));

  std::string_view unit = std::string_view(t).substr(numEnd);
  unit = trim(unit);
  double mult = 1;
  if (!unit.empty()) {
    switch (unit.front()) {
      case 'b': mult = 1; break;
      case 'k': mult = 1024.0; break;
      case 'm': mult = 1024.0 * 1024; break;
      case 'g': mult = 1024.0 * 1024 * 1024; break;
      case 't': mult = 1024.0 * 1024 * 1024 * 1024; break;
      default: throw ValueError(fmt::format("'{}' has an unknown unit (use K, M, G, T)", text));
    }
    const std::string_view rest = unit.substr(1);
    if (!(rest.empty() || rest == "b" || rest == "ib" || (unit.front() == 'b' && rest.empty())))
      throw ValueError(fmt::format("'{}' has an unknown unit (use K, M, G, T)", text));
  }
  const double bytes = number * mult;
  if (bytes >= 18446744073709551615.0) throw ValueError(fmt::format("'{}' is too large", text));
  return static_cast<uint64_t>(std::llround(bytes));
}

Resolution parseResolution(std::string_view text) {
  text = trim(text);
  if (const ResolutionPreset* p = findResolutionPreset(text)) return {p->width, p->height, std::string(p->name)};
  const std::string t = toLower(text);
  const size_t x = t.find('x');
  if (x == std::string::npos)
    throw ValueError(fmt::format("'{}' is not a preset (120p ... 160k) or a size like 3000x2000", text));
  const std::string_view sv = t;
  try {
    Resolution r;
    r.width = static_cast<uint32_t>(parseUint(sv.substr(0, x), kMinImageSide, kMaxImageSide));
    r.height = static_cast<uint32_t>(parseUint(sv.substr(x + 1), kMinImageSide, kMaxImageSide));
    r.preset.clear();
    return r;
  } catch (const ValueError&) {
    throw ValueError(
        fmt::format("'{}' is not a valid size: width and height must each be {} to {} px", text, kMinImageSide,
                    kMaxImageSide));
  }
}

PixelRect parseRect(std::string_view text) {
  const auto parts = split(text, ',');
  if (parts.size() != 4) throw ValueError(fmt::format("'{}' is not a region like x0,y0,x1,y1", text));
  PixelRect r;
  r.x0 = static_cast<uint32_t>(parseUint(parts[0], 0, kMaxImageSide));
  r.y0 = static_cast<uint32_t>(parseUint(parts[1], 0, kMaxImageSide));
  r.x1 = static_cast<uint32_t>(parseUint(parts[2], 0, kMaxImageSide));
  r.y1 = static_cast<uint32_t>(parseUint(parts[3], 0, kMaxImageSide));
  if (r.x0 >= r.x1 || r.y0 >= r.y1)
    throw ValueError(fmt::format("'{}' is empty: x1 must be greater than x0 and y1 greater than y0", text));
  return r;
}

SampleRange parseSampleRange(std::string_view text) {
  const auto parts = split(text, '-');
  if (parts.size() != 2) throw ValueError(fmt::format("'{}' is not a sample range like 0-63", text));
  SampleRange r;
  r.first = static_cast<uint32_t>(parseUint(parts[0], 0, 4294967294ull));
  r.last = static_cast<uint32_t>(parseUint(parts[1], 0, 4294967294ull));
  if (r.first > r.last) throw ValueError(fmt::format("'{}' ends before it starts", text));
  return r;
}

std::string formatDuration(uint64_t seconds) {
  if (seconds == 0) return "0s";
  std::string out;
  const uint64_t d = seconds / 86400, h = seconds / 3600 % 24, m = seconds / 60 % 60, s = seconds % 60;
  if (d) out += fmt::format("{}d", d);
  if (h) out += fmt::format("{}h", h);
  if (m) out += fmt::format("{}m", m);
  if (s) out += fmt::format("{}s", s);
  return out;
}

// Decimal units, like the guide's tables and disk vendors (241.6 GB for the 160K film).
std::string formatBytes(uint64_t bytes) {
  const double b = static_cast<double>(bytes);
  if (b >= 1e12) return fmt::format("{:.1f} TB", b / 1e12);
  if (b >= 1e9) return fmt::format("{:.1f} GB", b / 1e9);
  if (b >= 1e6) return fmt::format("{:.1f} MB", b / 1e6);
  if (b >= 1e3) return fmt::format("{:.1f} KB", b / 1e3);
  return fmt::format("{} B", bytes);
}

}  // namespace pen
