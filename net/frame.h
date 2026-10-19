#pragma once

// Frame layout (docs/GUIDE.md "Wire protocol", docs/PROTOCOL.md), all integers little-endian:
//   magic "PNBR" (4) | protocol version u16 | message type u16 | flags u16 | payload length u32 | payload
// Payloads are at most 16 MB. A larger message is sent as "part" frames (flag bit 1) of up to 4 MB, each
// starting with a 12-byte part header: transfer id u32 | total length u32 | offset u32. Control frames
// may be interleaved between the parts of one message, never inside a frame.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "net/protocol.h"

namespace pen::net {

inline constexpr size_t kHeaderBytes = 14;
inline constexpr size_t kPartHeaderBytes = 12;
inline constexpr uint32_t kMaxPayload = 16u << 20;
inline constexpr uint32_t kChunkBytes = 4u << 20;
inline constexpr uint32_t kMaxMessage = 512u << 20;  // a reassembled message (the largest tile is ~25 MB)

inline constexpr uint16_t kFlagZstd = 1u << 0;  // reserved: payload is zstd-compressed (unused in version 1)
inline constexpr uint16_t kFlagPart = 1u << 1;

struct FrameHeader {
  uint16_t version = 0;
  MsgType type{};
  uint16_t flags = 0;
  uint32_t length = 0;
};

std::array<uint8_t, kHeaderBytes> encodeHeader(const FrameHeader& h);
// Throws ProtocolError on a bad magic or a payload over the limit. The version is returned, not checked.
FrameHeader decodeHeader(std::span<const uint8_t, kHeaderBytes> bytes);

// One frame ready to write: header + payload in one buffer.
using FrameBytes = std::vector<uint8_t>;

// Splits a message into frames: one frame when it fits in kChunkBytes, else part frames.
std::vector<FrameBytes> makeFrames(MsgType type, std::span<const uint8_t> payload, uint32_t transferId);

// Reassembles part frames. feed() returns true when `out` holds a complete message.
class PartAssembler {
 public:
  // Throws ProtocolError on a malformed or out-of-order part.
  bool feed(MsgType type, std::span<const uint8_t> framePayload, Message& out);

 private:
  bool active_ = false;
  MsgType type_{};
  uint32_t transfer_ = 0, total_ = 0, received_ = 0;
  std::vector<uint8_t> buffer_;
};

// Little-endian helpers.
inline void putU16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
inline void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t getU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace pen::net
