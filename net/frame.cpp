#include "net/frame.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

#include "core/version.h"

namespace pen::net {

std::string_view toString(MsgType t) {
  switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::Welcome: return "Welcome";
    case MsgType::Reject: return "Reject";
    case MsgType::SceneOffer: return "SceneOffer";
    case MsgType::WorkerReady: return "WorkerReady";
    case MsgType::RequestWork: return "RequestWork";
    case MsgType::Lease: return "Lease";
    case MsgType::TileResult: return "TileResult";
    case MsgType::TileAck: return "TileAck";
    case MsgType::Release: return "Release";
    case MsgType::DeviceFailed: return "DeviceFailed";
    case MsgType::Cancel: return "Cancel";
    case MsgType::Pause: return "Pause";
    case MsgType::RenderComplete: return "RenderComplete";
    case MsgType::Stats: return "Stats";
    case MsgType::Heartbeat: return "Heartbeat";
    case MsgType::Log: return "Log";
    case MsgType::Bye: return "Bye";
    case MsgType::Kick: return "Kick";
  }
  return "unknown";
}

std::array<uint8_t, kHeaderBytes> encodeHeader(const FrameHeader& h) {
  std::array<uint8_t, kHeaderBytes> b{'P', 'N', 'B', 'R'};
  putU16(&b[4], h.version);
  putU16(&b[6], static_cast<uint16_t>(h.type));
  putU16(&b[8], h.flags);
  putU32(&b[10], h.length);
  return b;
}

FrameHeader decodeHeader(std::span<const uint8_t, kHeaderBytes> b) {
  if (std::memcmp(b.data(), "PNBR", 4) != 0)
    throw ProtocolError("not a Penumbra connection (bad frame magic)");
  FrameHeader h;
  h.version = getU16(&b[4]);
  h.type = static_cast<MsgType>(getU16(&b[6]));
  h.flags = getU16(&b[8]);
  h.length = getU32(&b[10]);
  if (h.length > kMaxPayload)
    throw ProtocolError(fmt::format("frame payload of {} bytes is over the 16 MB limit", h.length));
  return h;
}

namespace {
FrameBytes frame(MsgType type, uint16_t flags, std::span<const uint8_t> prefix, std::span<const uint8_t> body) {
  FrameBytes f(kHeaderBytes + prefix.size() + body.size());
  const auto h = encodeHeader({kProtocolVersion, type, flags, static_cast<uint32_t>(prefix.size() + body.size())});
  std::memcpy(f.data(), h.data(), kHeaderBytes);
  if (!prefix.empty()) std::memcpy(f.data() + kHeaderBytes, prefix.data(), prefix.size());
  if (!body.empty()) std::memcpy(f.data() + kHeaderBytes + prefix.size(), body.data(), body.size());
  return f;
}
}  // namespace

std::vector<FrameBytes> makeFrames(MsgType type, std::span<const uint8_t> payload, uint32_t transferId) {
  std::vector<FrameBytes> out;
  if (payload.size() <= kChunkBytes) {
    out.push_back(frame(type, 0, {}, payload));
    return out;
  }
  if (payload.size() > kMaxMessage)
    throw ProtocolError(fmt::format("message of {} bytes is over the {} byte limit", payload.size(), kMaxMessage));
  for (size_t offset = 0; offset < payload.size(); offset += kChunkBytes) {
    const size_t n = std::min<size_t>(kChunkBytes, payload.size() - offset);
    uint8_t part[kPartHeaderBytes];
    putU32(part, transferId);
    putU32(part + 4, static_cast<uint32_t>(payload.size()));
    putU32(part + 8, static_cast<uint32_t>(offset));
    out.push_back(frame(type, kFlagPart, part, payload.subspan(offset, n)));
  }
  return out;
}

bool PartAssembler::feed(MsgType type, std::span<const uint8_t> p, Message& out) {
  if (p.size() < kPartHeaderBytes) throw ProtocolError("part frame too short");
  const uint32_t transfer = getU32(p.data()), total = getU32(p.data() + 4), offset = getU32(p.data() + 8);
  const auto body = p.subspan(kPartHeaderBytes);
  if (!active_) {
    if (offset != 0) throw ProtocolError("part frame does not start at offset 0");
    if (total > kMaxMessage || total <= kChunkBytes) throw ProtocolError("bad part message size");
    active_ = true;
    type_ = type;
    transfer_ = transfer;
    total_ = total;
    received_ = 0;
    buffer_.assign(total, 0);
  }
  if (type != type_ || transfer != transfer_ || total != total_ || offset != received_ ||
      body.size() > total_ - received_)
    throw ProtocolError("part frames out of order");
  std::memcpy(buffer_.data() + received_, body.data(), body.size());
  received_ += static_cast<uint32_t>(body.size());
  if (received_ < total_) return false;
  out.type = type_;
  out.payload = std::move(buffer_);
  buffer_ = {};
  active_ = false;
  return true;
}

}  // namespace pen::net
