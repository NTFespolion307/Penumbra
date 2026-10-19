#include "net/tls.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <mutex>

namespace pen::net {

namespace {

const char kIdentity[] = "penumbra-join-v1";

std::string mbedError(int rc) {
  char buf[160];
  mbedtls_strerror(rc, buf, sizeof(buf));
  return fmt::format("{} (-0x{:04X})", buf, -rc);
}

void initPsa() {
  static std::once_flag once;
  static psa_status_t status = PSA_SUCCESS;
  std::call_once(once, [] { status = psa_crypto_init(); });
  if (status != PSA_SUCCESS) throw TlsError(fmt::format("psa_crypto_init failed ({})", int(status)), false);
}

// TLS 1.3 only: AES-256-GCM and ChaCha20-Poly1305, no fallback to older versions.
const int kCiphersuites[] = {MBEDTLS_TLS1_3_AES_256_GCM_SHA384, MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
                             MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 0};

}  // namespace

struct TlsSession::Impl {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  std::vector<uint8_t> in;  // ciphertext not yet consumed
  size_t in_pos = 0;
  std::vector<uint8_t> out;  // ciphertext to send
  bool established = false;
  bool peer_closed = false;

  static int sendCb(void* ctx, const unsigned char* buf, size_t len) {
    auto* self = static_cast<Impl*>(ctx);
    self->out.insert(self->out.end(), buf, buf + len);
    return static_cast<int>(len);
  }
  static int recvCb(void* ctx, unsigned char* buf, size_t len) {
    auto* self = static_cast<Impl*>(ctx);
    const size_t avail = self->in.size() - self->in_pos;
    if (avail == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    const size_t n = std::min(len, avail);
    std::memcpy(buf, self->in.data() + self->in_pos, n);
    self->in_pos += n;
    if (self->in_pos == self->in.size()) {
      self->in.clear();
      self->in_pos = 0;
    }
    return static_cast<int>(n);
  }

  Impl() {
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
  }
  ~Impl() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
};

TlsSession::TlsSession(Role role, const JoinKey& key) : impl_(std::make_unique<Impl>()) {
  initPsa();
  Impl& s = *impl_;
  const auto check = [](int rc, const char* what) {
    if (rc != 0) throw TlsError(fmt::format("TLS setup: {} failed: {}", what, mbedError(rc)), false);
  };
  const char pers[] = "penumbra";
  check(mbedtls_ctr_drbg_seed(&s.drbg, mbedtls_entropy_func, &s.entropy, reinterpret_cast<const unsigned char*>(pers),
                              sizeof(pers) - 1),
        "seeding the random generator");
  check(mbedtls_ssl_config_defaults(&s.conf, role == Role::Client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT),
        "config defaults");
  mbedtls_ssl_conf_rng(&s.conf, mbedtls_ctr_drbg_random, &s.drbg);
  mbedtls_ssl_conf_min_tls_version(&s.conf, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_max_tls_version(&s.conf, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_ciphersuites(&s.conf, kCiphersuites);
  mbedtls_ssl_conf_tls13_key_exchange_modes(&s.conf, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
  mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_NONE);  // the PSK authenticates both sides
  check(mbedtls_ssl_conf_psk(&s.conf, key.data(), key.size(), reinterpret_cast<const unsigned char*>(kIdentity),
                             sizeof(kIdentity) - 1),
        "setting the join key");
  check(mbedtls_ssl_setup(&s.ssl, &s.conf), "session setup");
  mbedtls_ssl_set_bio(&s.ssl, &s, &Impl::sendCb, &Impl::recvCb, nullptr);
}

TlsSession::~TlsSession() = default;

void TlsSession::feed(std::span<const uint8_t> data) {
  impl_->in.insert(impl_->in.end(), data.begin(), data.end());
}

bool TlsSession::handshake() {
  if (impl_->established) return true;
  const int rc = mbedtls_ssl_handshake(&impl_->ssl);
  if (rc == 0) {
    impl_->established = true;
    return true;
  }
  if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return false;
  throw TlsError(fmt::format("TLS handshake failed: {}", mbedError(rc)), true);
}

bool TlsSession::established() const { return impl_->established; }

void TlsSession::write(std::span<const uint8_t> plain) {
  size_t done = 0;
  while (done < plain.size()) {
    const int rc = mbedtls_ssl_write(&impl_->ssl, plain.data() + done, plain.size() - done);
    if (rc < 0) throw TlsError(fmt::format("TLS write failed: {}", mbedError(rc)), false);
    done += static_cast<size_t>(rc);
  }
}

bool TlsSession::read(std::vector<uint8_t>& out) {
  if (impl_->peer_closed) return false;
  unsigned char buf[16384];
  for (;;) {
    const int rc = mbedtls_ssl_read(&impl_->ssl, buf, sizeof(buf));
    if (rc > 0) {
      out.insert(out.end(), buf, buf + rc);
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    // A TLS 1.3 server may send session tickets after the handshake; Penumbra does not use them.
    if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      impl_->peer_closed = true;
      return false;
    }
    throw TlsError(fmt::format("TLS read failed: {}", mbedError(rc)), false);
  }
}

void TlsSession::close() { mbedtls_ssl_close_notify(&impl_->ssl); }

std::vector<uint8_t> TlsSession::takeOutput() {
  std::vector<uint8_t> v;
  v.swap(impl_->out);
  return v;
}

}  // namespace pen::net
