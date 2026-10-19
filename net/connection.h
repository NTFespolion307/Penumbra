#pragma once

// One TCP connection carrying framed messages (docs/GUIDE.md "Wire protocol"). All socket work runs on an
// IoThread; send() and close() may be called from any thread. Two send queues: control messages always go
// next, bulk messages (tiles, scene data) are split into 4 MB parts, so a control frame never waits behind
// more than one part.

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "net/frame.h"

namespace pen::net {

// An io_context running on its own thread until stop() or destruction.
class IoThread {
 public:
  IoThread();
  ~IoThread();
  IoThread(const IoThread&) = delete;
  IoThread& operator=(const IoThread&) = delete;
  asio::io_context& context() { return io_; }
  void stop();

 private:
  asio::io_context io_;
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  std::thread thread_;
};

enum class Priority { Control, Bulk };

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  struct Handlers {
    std::function<void(Message&& msg, uint16_t version)> message;  // on the io thread, in arrival order
    std::function<void(const std::string& reason)> closed;          // on the io thread, exactly once
  };

  explicit Connection(asio::ip::tcp::socket socket);
  ~Connection();

  // Begins reading. Handlers must be set before any message can arrive.
  void start(Handlers handlers);

  void send(MsgType type, std::vector<uint8_t> payload, Priority priority = Priority::Control);
  template <class NativeT>
  void send(const NativeT& msg, Priority priority = Priority::Control) {
    send(MsgTypeOf<typename NativeT::TableType>::value, encode(msg), priority);
  }

  // Closes at once; queued frames are dropped. The closed handler runs with `reason`.
  void close(const std::string& reason);
  // Closes after every queued frame is written.
  void closeAfterSend(const std::string& reason);

  bool isOpen() const { return open_.load(); }
  uint64_t queuedBytes() const { return queued_.load(); }
  const std::string& peer() const { return peer_; }        // "192.168.1.20:51234"
  const std::string& peerAddress() const { return address_; }  // "192.168.1.20"

 private:
  void readHeader();
  void readPayload(const FrameHeader& h);
  void writeNext();
  void fail(const std::string& reason);

  asio::ip::tcp::socket socket_;
  Handlers handlers_;
  std::string peer_, address_;
  std::atomic<bool> open_{true};
  std::atomic<uint64_t> queued_{0};
  std::atomic<uint32_t> next_transfer_{1};

  std::array<uint8_t, kHeaderBytes> header_{};
  std::vector<uint8_t> payload_;
  PartAssembler parts_;

  std::mutex mutex_;  // guards the queues and the flags below
  std::deque<FrameBytes> control_, bulk_;
  FrameBytes writing_frame_;
  bool writing_ = false;
  bool close_after_send_ = false;
  std::string close_reason_;
};

// Raised when connecting fails; `refused` is true when nothing listens at the address (yet).
struct ConnectError : std::runtime_error {
  ConnectError(const std::string& msg, bool refused) : std::runtime_error(msg), refused(refused) {}
  bool refused;
};

// Resolves `host` and connects (blocking). Throws ConnectError.
std::shared_ptr<Connection> connectTo(IoThread& io, const std::string& host, uint16_t port);

// Accepts connections on bind:port (all interfaces when bind is empty, IPv6 and IPv4 where possible).
class Listener {
 public:
  using AcceptFn = std::function<void(std::shared_ptr<Connection>)>;  // on the io thread
  // Throws std::runtime_error when the address cannot be bound (port in use, bad address).
  Listener(IoThread& io, const std::string& bind, uint16_t port, AcceptFn onAccept);
  ~Listener();
  uint16_t port() const { return port_; }
  void stop();

 private:
  void acceptNext();
  asio::ip::tcp::acceptor acceptor_;
  AcceptFn on_accept_;
  uint16_t port_ = 0;
  std::atomic<bool> stopped_{false};
};

// "host", "host:port", "[v6]:port" or a bare IPv6 address. Throws std::invalid_argument.
struct JoinAddress {
  std::string host;
  uint16_t port = 7420;
};
JoinAddress parseJoinAddress(const std::string& text, uint16_t defaultPort = 7420);

}  // namespace pen::net
