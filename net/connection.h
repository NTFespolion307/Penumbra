#pragma once

#include <chrono>

// One TLS-encrypted TCP connection carrying framed messages (docs/GUIDE.md "Wire protocol", "Security"). The
// TLS 1.3 handshake uses the key derived from the join code; a peer with another code cannot connect. All
// socket and TLS work runs on an IoThread; send() and close() may be called from any thread. Two send queues:
// control messages always go next, bulk messages (tiles, scene data) are split into 4 MB parts, so a control
// frame never waits behind more than one part.

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
#include <vector>

#include "net/frame.h"
#include "net/tls.h"

namespace pen::net {

// An io_context running on `threads` threads until stop() or destruction. Each connection gets its own strand, so
// one connection's work stays in order while different connections are handled in parallel: a controller with many
// workers decrypts and unpacks their tiles on several cores (M9, D-112).
class IoThread {
 public:
  explicit IoThread(unsigned threads = 1);
  ~IoThread();
  IoThread(const IoThread&) = delete;
  IoThread& operator=(const IoThread&) = delete;
  asio::io_context& context() { return io_; }
  void stop();

 private:
  asio::io_context io_;
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  std::vector<std::thread> threads_;
};

enum class Priority { Control, Bulk };

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  struct Handlers {
    std::function<void(Message&& msg, uint16_t version)> message;  // on the io thread, in arrival order
    std::function<void(const std::string& reason)> closed;          // on the io thread, exactly once
  };

  Connection(asio::ip::tcp::socket socket, TlsSession::Role role, const JoinKey& key);
  ~Connection();

  // Starts the TLS handshake and reading. Messages may be sent before the handshake completes; they wait.
  void start(Handlers handlers);

  void send(MsgType type, std::vector<uint8_t> payload, Priority priority = Priority::Control);
  template <class NativeT>
  void send(const NativeT& msg, Priority priority = Priority::Control) {
    send(MsgTypeOf<typename NativeT::TableType>::value, encode(msg), priority);
  }

  // Runs `fn` on this connection's strand, in order with its message handlers.
  template <class F>
  void post(F&& fn) {
    asio::post(socket_.get_executor(), std::forward<F>(fn));
  }

  // Closes at once; queued frames are dropped. The closed handler runs with `reason`.
  void close(const std::string& reason);
  // Closes after every queued frame is written.
  void closeAfterSend(const std::string& reason);

  bool isOpen() const { return open_.load(); }
  // When the last bytes arrived from the peer (heartbeats included); the connection's start before that.
  std::chrono::steady_clock::time_point lastReceived() const {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_rx_.load()));
  }
  // The TLS handshake failed (almost always a wrong join code) or timed out.
  bool handshakeFailed() const { return handshake_failed_.load(); }
  // Closed before the other side sent a single byte: a wrong code gets a TLS alert, so this is a relay, firewall
  // or something other than Penumbra dropping the connection (or a controller ignoring this address).
  bool closedBeforeReply() const { return handshake_failed_.load() && !received_.load(); }
  uint64_t queuedBytes() const { return queued_.load(); }
  uint64_t bytesSent() const { return sent_.load(); }  // on the wire (encrypted), since the connection opened
  const std::string& peer() const { return peer_; }        // "192.168.1.20:51234"
  const std::string& peerAddress() const { return address_; }  // "192.168.1.20"

 private:
  void readSome();
  void onCipher(size_t n);
  void parseFrames();
  void writeNext();
  void fail(const std::string& reason);

  asio::ip::tcp::socket socket_;
  asio::steady_timer handshake_timer_;
  TlsSession tls_;  // used on the io thread only
  Handlers handlers_;
  std::string peer_, address_;
  std::atomic<bool> open_{true};
  std::atomic<bool> established_{false};
  std::atomic<bool> handshake_failed_{false};
  std::atomic<bool> received_{false};  // the other side has sent at least one byte
  std::atomic<uint64_t> queued_{0};
  std::atomic<uint64_t> sent_{0};
  std::atomic<std::chrono::steady_clock::rep> last_rx_{std::chrono::steady_clock::now().time_since_epoch().count()};
  std::atomic<uint32_t> next_transfer_{1};

  std::array<uint8_t, 65536> cipher_in_{};
  std::vector<uint8_t> plain_;  // decrypted bytes not yet parsed into frames
  size_t plain_pos_ = 0;
  PartAssembler parts_;

  std::mutex mutex_;  // guards the queues and the flags below
  std::deque<FrameBytes> control_, bulk_;
  std::vector<uint8_t> writing_;  // ciphertext being written
  uint64_t writing_plain_ = 0;     // plaintext bytes it carries (for queuedBytes)
  bool busy_ = false;              // a write is in progress or posted
  bool close_after_send_ = false;
  bool close_notified_ = false;
  std::string close_reason_;
};

// Raised when connecting fails; `refused` is true when nothing listens at the address (yet).
struct ConnectError : std::runtime_error {
  ConnectError(const std::string& msg, bool refused) : std::runtime_error(msg), refused(refused) {}
  bool refused;
};

inline constexpr std::chrono::seconds kConnectTimeout{10};

// Resolves `host` and connects (blocking, at most kConnectTimeout); the TLS handshake starts with
// Connection::start. Throws ConnectError.
std::shared_ptr<Connection> connectTo(IoThread& io, const std::string& host, uint16_t port, const JoinKey& key);

// Accepts connections on bind:port (all interfaces when bind is empty, IPv6 and IPv4 where possible).
class Listener {
 public:
  using AcceptFn = std::function<void(std::shared_ptr<Connection>)>;  // on the io thread
  // Throws std::runtime_error when the address cannot be bound (port in use, bad address).
  Listener(IoThread& io, const std::string& bind, uint16_t port, const JoinKey& key, AcceptFn onAccept);
  ~Listener();
  uint16_t port() const { return port_; }
  void stop();

 private:
  void acceptNext();
  asio::io_context& io_;
  asio::ip::tcp::acceptor acceptor_;
  JoinKey key_;
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
