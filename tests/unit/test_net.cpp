#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>

#include "core/version.h"
#include "net/connection.h"
#include "net/frame.h"
#include "net/join_code.h"
#include "net/protocol.h"
#include "net/tile_codec.h"

using namespace pen;
using namespace pen::net;

TEST_CASE("frame header round-trips and rejects garbage", "[net]") {
  const FrameHeader h{kProtocolVersion, MsgType::TileResult, kFlagPart, 123456};
  const auto bytes = encodeHeader(h);
  CHECK(std::memcmp(bytes.data(), "PNBR", 4) == 0);
  const FrameHeader back = decodeHeader(bytes);
  CHECK(back.version == h.version);
  CHECK(back.type == h.type);
  CHECK(back.flags == h.flags);
  CHECK(back.length == h.length);

  auto bad = bytes;
  bad[0] = 'X';
  CHECK_THROWS_AS(decodeHeader(bad), ProtocolError);
  auto huge = encodeHeader({kProtocolVersion, MsgType::Lease, 0, kMaxPayload + 1});
  CHECK_THROWS_AS(decodeHeader(huge), ProtocolError);
}

TEST_CASE("large messages are split into 4 MB parts and reassembled", "[net]") {
  std::vector<uint8_t> payload(kChunkBytes * 2 + 12345);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + (i >> 13));
  const auto frames = makeFrames(MsgType::TileResult, payload, 42);
  REQUIRE(frames.size() == 3);

  PartAssembler asmb;
  Message out;
  for (size_t i = 0; i < frames.size(); ++i) {
    const FrameHeader h = decodeHeader(std::span<const uint8_t, kHeaderBytes>(frames[i].data(), kHeaderBytes));
    CHECK((h.flags & kFlagPart) != 0);
    const bool complete =
        asmb.feed(h.type, std::span(frames[i]).subspan(kHeaderBytes), out);
    CHECK(complete == (i + 1 == frames.size()));
  }
  CHECK(out.type == MsgType::TileResult);
  CHECK(out.payload == payload);

  // A small message is one plain frame.
  const auto small = makeFrames(MsgType::Lease, std::span(payload).first(100), 1);
  REQUIRE(small.size() == 1);
  CHECK(small[0].size() == kHeaderBytes + 100);

  // Parts out of order are refused.
  PartAssembler asmb2;
  CHECK_THROWS_AS(asmb2.feed(MsgType::TileResult, std::span(frames[1]).subspan(kHeaderBytes), out), ProtocolError);
}

TEST_CASE("messages encode and decode; malformed payloads are refused", "[net]") {
  wire::HelloT hello;
  hello.protocol_version = kProtocolVersion;
  hello.app_version = "0.1.0";
  hello.git_hash = "abc";
  hello.worker_name = "gpu-server-1";
  auto dev = std::make_unique<wire::DeviceDescT>();
  dev->name = "RTX 5060";
  dev->memory_bytes = 8ull << 30;
  hello.devices.push_back(std::move(dev));
  const auto bytes = encode(hello);
  const auto back = decode<wire::HelloT>(bytes);
  CHECK(back.worker_name == "gpu-server-1");
  REQUIRE(back.devices.size() == 1);
  CHECK(back.devices[0]->memory_bytes == (8ull << 30));
  CHECK(MsgTypeOf<wire::Hello>::value == MsgType::Hello);

  std::vector<uint8_t> garbage(64, 0xEE);
  CHECK_THROWS_AS(decode<wire::HelloT>(garbage), ProtocolError);
  CHECK_THROWS_AS(decode<wire::HelloT>(std::span(bytes).first(bytes.size() / 2)), ProtocolError);
}

TEST_CASE("tile pixels survive the wire bit for bit", "[net]") {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.f, 4.f);
  std::vector<Vec3f> mean(257 * 131);
  for (auto& v : mean) v = {u(rng), u(rng) * 1e-6f, u(rng) * 1e6f};
  mean[0] = {0.f, -0.f, std::numeric_limits<float>::denorm_min()};
  mean[1] = {70000.f, 1e30f, std::numeric_limits<float>::max()};  // beyond float16: needs float32

  const auto data = encodeTilePixels(mean);
  CHECK(data.size() < mean.size() * sizeof(Vec3f));
  const auto back = decodeTilePixels(data, mean.size());
  REQUIRE(back.size() == mean.size());
  CHECK(std::memcmp(back.data(), mean.data(), mean.size() * sizeof(Vec3f)) == 0);
  CHECK(tileChecksum(back) == tileChecksum(mean));

  CHECK_THROWS_AS(decodeTilePixels(data, mean.size() + 1), ProtocolError);
  auto corrupt = data;
  corrupt[corrupt.size() / 2] ^= 0x55;
  bool caught = false;
  try {
    const auto c = decodeTilePixels(corrupt, mean.size());
    caught = tileChecksum(c) != tileChecksum(mean);  // decoded, but the checksum catches it
  } catch (const ProtocolError&) {
    caught = true;
  }
  CHECK(caught);
}

TEST_CASE("join addresses parse with and without a port", "[net]") {
  CHECK(parseJoinAddress("192.168.1.20").host == "192.168.1.20");
  CHECK(parseJoinAddress("192.168.1.20").port == 7420);
  CHECK(parseJoinAddress("203.0.113.5:7421").port == 7421);
  CHECK(parseJoinAddress("gpu-server.local:9000").host == "gpu-server.local");
  CHECK(parseJoinAddress("[fd7a:115c::1]:7420").host == "fd7a:115c::1");
  CHECK(parseJoinAddress("fd7a:115c::1").host == "fd7a:115c::1");
  CHECK(parseJoinAddress("fd7a:115c::1").port == 7420);
  CHECK_THROWS(parseJoinAddress(""));
  CHECK_THROWS(parseJoinAddress("host:0"));
  CHECK_THROWS(parseJoinAddress("host:70000"));
  CHECK_THROWS(parseJoinAddress("host:abc"));
  CHECK_THROWS(parseJoinAddress("[::1"));
}

TEST_CASE("a loopback connection delivers control frames between bulk parts", "[net]") {
  IoThread io;
  std::mutex m;
  std::condition_variable cv;
  std::vector<Message> got;
  std::shared_ptr<Connection> server;
  const JoinKey key = deriveJoinKey("K7QDM2XR9FHT4WNB");
  Listener listener(io, "127.0.0.1", 0, key, [&](std::shared_ptr<Connection> c) {
    std::lock_guard lock(m);
    server = c;
    c->start({[&](Message&& msg, uint16_t) {
                std::lock_guard lock2(m);
                got.push_back(std::move(msg));
                cv.notify_all();
              },
              [](const std::string&) {}});
  });
  auto client = connectTo(io, "127.0.0.1", listener.port(), key);
  client->start({[](Message&&, uint16_t) {}, [](const std::string&) {}});

  std::vector<uint8_t> big(kChunkBytes * 3, 0xAB);
  client->send(MsgType::TileResult, big, Priority::Bulk);
  wire::LeaseT lease;
  lease.lease_id = 77;
  client->send(lease);

  std::unique_lock lock(m);
  REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return got.size() == 2; }));
  // The lease was queued after the 12 MB result but must not wait for all of it.
  CHECK(got[0].type == MsgType::Lease);
  CHECK(decode<wire::LeaseT>(got[0].payload).lease_id == 77);
  CHECK(got[1].type == MsgType::TileResult);
  CHECK(got[1].payload == big);
  lock.unlock();
  client->close("done");
  if (server) server->close("done");
  listener.stop();
}

// ---------------------------------------------------------------- TLS and join codes


namespace {
// Moves ciphertext between two sessions until neither has anything to send.
void pump(TlsSession& a, TlsSession& b) {
  for (int i = 0; i < 64; ++i) {
    auto ab = a.takeOutput();
    auto ba = b.takeOutput();
    if (ab.empty() && ba.empty()) return;
    b.feed(ab);
    a.feed(ba);
    if (!a.established()) a.handshake();
    if (!b.established()) b.handshake();
  }
}
}  // namespace

TEST_CASE("join codes normalise, format and derive keys", "[net]") {
  const std::string code = generateJoinCode();
  CHECK(code.size() == 16);
  CHECK(normalizeJoinCode(code) == code);
  CHECK(normalizeJoinCode("k7qd-m2xr-9fht-4wnb") == "K7QDM2XR9FHT4WNB");
  CHECK(normalizeJoinCode("K7QD M2XR 9FHT 4WNB") == "K7QDM2XR9FHT4WNB");
  CHECK(normalizeJoinCode("IOLI-0000-0000-0000") == "1011000000000000");  // Crockford: I, L -> 1, O -> 0
  CHECK_FALSE(normalizeJoinCode("K7QD-M2XR-9FHT-4WN").has_value());     // too short
  CHECK_FALSE(normalizeJoinCode("K7QD-M2XR-9FHT-4WNU").has_value());    // U is not Crockford
  CHECK(formatJoinCode("K7QDM2XR9FHT4WNB") == "K7QD-M2XR-9FHT-4WNB");
  CHECK(deriveJoinKey("K7QDM2XR9FHT4WNB") == deriveJoinKey("K7QDM2XR9FHT4WNB"));
  CHECK(deriveJoinKey("K7QDM2XR9FHT4WNB") != deriveJoinKey("K7QDM2XR9FHT4WNC"));
  CHECK(generateJoinCode() != generateJoinCode());
}

TEST_CASE("TLS 1.3 with the join key: same code connects, data round-trips", "[net]") {
  const JoinKey key = deriveJoinKey("K7QDM2XR9FHT4WNB");
  TlsSession client(TlsSession::Role::Client, key), server(TlsSession::Role::Server, key);
  client.handshake();
  pump(client, server);
  REQUIRE(client.established());
  REQUIRE(server.established());

  std::vector<uint8_t> msg(100000);
  for (size_t i = 0; i < msg.size(); ++i) msg[i] = uint8_t(i * 31);
  client.write(msg);
  const auto wire = client.takeOutput();
  CHECK(wire.size() > msg.size());  // records add headers and tags
  CHECK(std::search(wire.begin(), wire.end(), msg.begin(), msg.begin() + 64) == wire.end());  // not plaintext
  server.feed(wire);
  std::vector<uint8_t> got;
  CHECK(server.read(got));
  CHECK(got == msg);

  server.close();
  client.feed(server.takeOutput());
  std::vector<uint8_t> none;
  CHECK_FALSE(client.read(none));
}

TEST_CASE("TLS with a different join code cannot complete the handshake", "[net]") {
  TlsSession client(TlsSession::Role::Client, deriveJoinKey("K7QDM2XR9FHT4WNB"));
  TlsSession server(TlsSession::Role::Server, deriveJoinKey("AAAAAAAAAAAAAAAA"));
  client.handshake();
  bool failed = false;
  try {
    pump(client, server);
  } catch (const TlsError& e) {
    failed = e.during_handshake;
  }
  CHECK(failed);
  CHECK_FALSE(server.established());
}

TEST_CASE("a worker with the wrong join code cannot connect over a real socket", "[net]") {
  IoThread io;
  std::mutex m;
  std::condition_variable cv;
  std::shared_ptr<Connection> server;
  bool serverClosed = false, clientClosed = false, gotMessage = false;
  Listener listener(io, "127.0.0.1", 0, deriveJoinKey("K7QDM2XR9FHT4WNB"), [&](std::shared_ptr<Connection> c) {
    std::lock_guard lock(m);
    server = c;
    c->start({[&](Message&&, uint16_t) { gotMessage = true; },
              [&](const std::string&) {
                std::lock_guard lock2(m);
                serverClosed = true;
                cv.notify_all();
              }});
  });
  auto client = connectTo(io, "127.0.0.1", listener.port(), deriveJoinKey("AAAAAAAAAAAAAAAA"));
  client->start({[&](Message&&, uint16_t) { gotMessage = true; },
                 [&](const std::string&) {
                   std::lock_guard lock(m);
                   clientClosed = true;
                   cv.notify_all();
                 }});
  wire::HelloT hello;
  hello.worker_name = "intruder";
  client->send(hello);

  std::unique_lock lock(m);
  REQUIRE(cv.wait_for(lock, std::chrono::seconds(15), [&] { return serverClosed && clientClosed; }));
  CHECK(server->handshakeFailed());
  CHECK(client->handshakeFailed());
  CHECK_FALSE(gotMessage);
  lock.unlock();
  listener.stop();
}
