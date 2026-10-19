#pragma once

// Message types and payload encode/decode for the wire protocol (docs/PROTOCOL.md). The payload of every
// frame is one FlatBuffers table from net/schema/penumbra.fbs; the frame's type field names the table.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "penumbra_generated.h"

namespace pen::net {

struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class MsgType : uint16_t {
  Hello = 1,
  Welcome = 2,
  Reject = 3,
  SceneOffer = 10,
  SceneHave = 11,
  SceneChunk = 12,
  WorkerReady = 13,
  RequestWork = 20,
  Lease = 21,
  TileResult = 22,
  TileAck = 23,
  Release = 24,
  DeviceFailed = 25,
  Cancel = 30,
  Pause = 31,
  RenderComplete = 32,
  Stats = 40,
  Heartbeat = 41,
  Log = 42,
  Bye = 50,
  Kick = 51,
};

std::string_view toString(MsgType t);

// Table type -> message type.
template <class T> struct MsgTypeOf;
#define PEN_MSG(T) \
  template <> struct MsgTypeOf<wire::T> { static constexpr MsgType value = MsgType::T; };
PEN_MSG(Hello) PEN_MSG(Welcome) PEN_MSG(Reject) PEN_MSG(SceneOffer) PEN_MSG(SceneHave) PEN_MSG(SceneChunk)
PEN_MSG(WorkerReady) PEN_MSG(RequestWork)
PEN_MSG(Lease) PEN_MSG(TileResult) PEN_MSG(TileAck) PEN_MSG(Release) PEN_MSG(DeviceFailed) PEN_MSG(Cancel)
PEN_MSG(Pause) PEN_MSG(RenderComplete) PEN_MSG(Stats) PEN_MSG(Heartbeat) PEN_MSG(Log) PEN_MSG(Bye) PEN_MSG(Kick)
#undef PEN_MSG

// A received message: its type and FlatBuffers payload.
struct Message {
  MsgType type{};
  std::vector<uint8_t> payload;
};

// Serializes a native (object API) table.
template <class NativeT>
std::vector<uint8_t> encode(const NativeT& msg) {
  flatbuffers::FlatBufferBuilder fbb(256);
  fbb.Finish(NativeT::TableType::Pack(fbb, &msg));
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// Verifies and unpacks a payload; throws ProtocolError when it is malformed.
template <class NativeT>
NativeT decode(std::span<const uint8_t> payload) {
  using Table = typename NativeT::TableType;
  flatbuffers::Verifier verifier(payload.data(), payload.size());
  if (!verifier.VerifyBuffer<Table>(nullptr)) throw ProtocolError("malformed message payload");
  NativeT out;
  flatbuffers::GetRoot<Table>(payload.data())->UnPackTo(&out);
  return out;
}

}  // namespace pen::net
