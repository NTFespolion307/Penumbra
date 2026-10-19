#pragma once

// TLS 1.3 with an external pre-shared key derived from the join code (docs/GUIDE.md "Security"), on memory
// buffers: the connection feeds received ciphertext in and sends what takeOutput() returns, so the socket
// stays fully asynchronous. Key exchange is PSK + ephemeral ECDHE only (forward secrecy); a peer with another
// key cannot complete the handshake. One session is used from one thread at a time.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pen::net {

using JoinKey = std::array<uint8_t, 32>;

struct TlsError : std::runtime_error {
  TlsError(const std::string& msg, bool handshake) : std::runtime_error(msg), during_handshake(handshake) {}
  bool during_handshake;  // a handshake failure usually means a wrong join code
};

class TlsSession {
 public:
  enum class Role { Client, Server };

  TlsSession(Role role, const JoinKey& key);
  ~TlsSession();
  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;

  // Ciphertext received from the peer.
  void feed(std::span<const uint8_t> data);
  // Advances the handshake as far as the received data allows; true once it is complete. Throws TlsError.
  bool handshake();
  bool established() const;
  // Encrypts plaintext (after the handshake). Throws TlsError.
  void write(std::span<const uint8_t> plain);
  // Decrypts every complete record received so far and appends the plaintext. Returns false once the peer
  // has closed the session (close_notify). Throws TlsError.
  bool read(std::vector<uint8_t>& out);
  // Queues a close_notify.
  void close();
  // Ciphertext to send now (empty when there is none).
  std::vector<uint8_t> takeOutput();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pen::net
