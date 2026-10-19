#include "net/connection.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fmt/format.h>
#include <future>

namespace pen::net {

// ---------------------------------------------------------------- IoThread

IoThread::IoThread() : guard_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}

IoThread::~IoThread() { stop(); }

void IoThread::stop() {
  guard_.reset();
  io_.stop();
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

// ---------------------------------------------------------------- Connection

namespace {
std::string endpointText(const asio::ip::tcp::endpoint& ep) {
  const asio::ip::address a = ep.address();
  if (a.is_v6()) {
    const auto v6 = a.to_v6();
    if (v6.is_v4_mapped()) return fmt::format("{}:{}", asio::ip::make_address_v4(asio::ip::v4_mapped, v6).to_string(), ep.port());
    return fmt::format("[{}]:{}", a.to_string(), ep.port());
  }
  return fmt::format("{}:{}", a.to_string(), ep.port());
}
std::string addressText(const asio::ip::tcp::endpoint& ep) {
  const asio::ip::address a = ep.address();
  if (a.is_v6() && a.to_v6().is_v4_mapped())
    return asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()).to_string();
  return a.to_string();
}
}  // namespace

Connection::Connection(asio::ip::tcp::socket socket) : socket_(std::move(socket)) {
  std::error_code ec;
  socket_.set_option(asio::ip::tcp::no_delay(true), ec);
  socket_.set_option(asio::socket_base::keep_alive(true), ec);
  const auto ep = socket_.remote_endpoint(ec);
  peer_ = ec ? "unknown" : endpointText(ep);
  address_ = ec ? "unknown" : addressText(ep);
}

Connection::~Connection() {
  std::error_code ec;
  socket_.close(ec);
}

void Connection::start(Handlers handlers) {
  handlers_ = std::move(handlers);
  asio::post(socket_.get_executor(), [self = shared_from_this()] { self->readHeader(); });
}

void Connection::readHeader() {
  asio::async_read(socket_, asio::buffer(header_), [self = shared_from_this()](std::error_code ec, size_t) {
    if (ec) return self->fail(ec == asio::error::eof ? "the other side closed the connection" : ec.message());
    FrameHeader h;
    try {
      h = decodeHeader(self->header_);
    } catch (const ProtocolError& e) {
      return self->fail(e.what());
    }
    self->readPayload(h);
  });
}

void Connection::readPayload(const FrameHeader& h) {
  payload_.resize(h.length);
  asio::async_read(socket_, asio::buffer(payload_), [self = shared_from_this(), h](std::error_code ec, size_t) {
    if (ec) return self->fail(ec == asio::error::eof ? "the other side closed the connection" : ec.message());
    if (!self->open_) return;
    Message msg;
    try {
      if (h.flags & kFlagPart) {
        if (!self->parts_.feed(h.type, self->payload_, msg)) return self->readHeader();
      } else {
        msg.type = h.type;
        msg.payload = std::move(self->payload_);
        self->payload_ = {};
      }
    } catch (const ProtocolError& e) {
      return self->fail(e.what());
    }
    if (self->handlers_.message) self->handlers_.message(std::move(msg), h.version);
    if (self->open_) self->readHeader();
  });
}

void Connection::send(MsgType type, std::vector<uint8_t> payload, Priority priority) {
  if (!open_) return;
  if (payload.size() > kChunkBytes) priority = Priority::Bulk;  // only bulk messages are ever split
  std::vector<FrameBytes> frames = makeFrames(type, payload, next_transfer_++);
  bool kick = false;
  {
    std::lock_guard lock(mutex_);
    if (close_after_send_) return;
    for (auto& f : frames) {
      queued_ += f.size();
      (priority == Priority::Control ? control_ : bulk_).push_back(std::move(f));
    }
    if (!writing_) kick = writing_ = true;
  }
  if (kick) asio::post(socket_.get_executor(), [self = shared_from_this()] { self->writeNext(); });
}

void Connection::writeNext() {
  {
    std::lock_guard lock(mutex_);
    if (!open_) {
      writing_ = false;
      return;
    }
    auto& q = !control_.empty() ? control_ : bulk_;
    if (q.empty()) {
      writing_ = false;
      if (close_after_send_) {
        std::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
        // The peer closes its side after reading everything; reading then ends with eof and fail() runs.
      }
      return;
    }
    writing_frame_ = std::move(q.front());
    q.pop_front();
  }
  asio::async_write(socket_, asio::buffer(writing_frame_), [self = shared_from_this()](std::error_code ec, size_t n) {
    self->queued_ -= n;
    if (ec) return self->fail(ec.message());
    self->writeNext();
  });
}

void Connection::fail(const std::string& reason) {
  if (!open_.exchange(false)) return;
  std::string why = reason;
  {
    std::lock_guard lock(mutex_);
    if (!close_reason_.empty()) why = close_reason_;
    control_.clear();
    bulk_.clear();
    queued_ = 0;
  }
  std::error_code ec;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
  socket_.close(ec);
  if (handlers_.closed) handlers_.closed(why);
}

void Connection::close(const std::string& reason) {
  asio::post(socket_.get_executor(), [self = shared_from_this(), reason] { self->fail(reason); });
}

void Connection::closeAfterSend(const std::string& reason) {
  bool kick = false;
  {
    std::lock_guard lock(mutex_);
    if (close_after_send_) return;
    close_after_send_ = true;
    close_reason_ = reason;
    if (!writing_) kick = writing_ = true;
  }
  if (kick) asio::post(socket_.get_executor(), [self = shared_from_this()] { self->writeNext(); });
}

// ---------------------------------------------------------------- connect / listen

std::shared_ptr<Connection> connectTo(IoThread& io, const std::string& host, uint16_t port) {
  asio::ip::tcp::resolver resolver(io.context());
  std::error_code ec;
  const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
  if (ec) throw ConnectError(fmt::format("cannot resolve '{}': {}", host, ec.message()), false);
  asio::ip::tcp::socket socket(io.context());
  asio::connect(socket, endpoints, ec);
  if (ec) {
    const bool refused = ec == asio::error::connection_refused;
    throw ConnectError(fmt::format("cannot connect to {}:{}: {}", host, port, ec.message()), refused);
  }
  return std::make_shared<Connection>(std::move(socket));
}

Listener::Listener(IoThread& io, const std::string& bind, uint16_t port, AcceptFn onAccept)
    : acceptor_(io.context()), on_accept_(std::move(onAccept)) {
  std::error_code ec;
  asio::ip::tcp::endpoint ep;
  if (bind.empty()) {
    ep = asio::ip::tcp::endpoint(asio::ip::tcp::v6(), port);
    acceptor_.open(ep.protocol(), ec);
    if (!ec) acceptor_.set_option(asio::ip::v6_only(false), ec);
    if (ec) {  // no IPv6 on this host
      ec.clear();
      if (acceptor_.is_open()) acceptor_.close(ec);
      ep = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
      acceptor_.open(ep.protocol(), ec);
    }
  } else {
    const auto addr = asio::ip::make_address(bind, ec);
    if (ec) throw std::runtime_error(fmt::format("--bind {}: not an IP address", bind));
    ep = asio::ip::tcp::endpoint(addr, port);
    acceptor_.open(ep.protocol(), ec);
  }
  if (ec) throw std::runtime_error(fmt::format("cannot open a socket: {}", ec.message()));
#if !defined(_WIN32)
  acceptor_.set_option(asio::socket_base::reuse_address(true), ec);  // on Windows this would allow port stealing
#endif
  acceptor_.bind(ep, ec);
  if (ec)
    throw std::runtime_error(fmt::format("cannot listen on port {}: {} (is another controller running? use --port)",
                                         port, ec.message()));
  acceptor_.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) throw std::runtime_error(fmt::format("cannot listen on port {}: {}", port, ec.message()));
  port_ = acceptor_.local_endpoint().port();
  asio::post(acceptor_.get_executor(), [this] { acceptNext(); });
}

Listener::~Listener() { stop(); }

void Listener::stop() {
  if (stopped_.exchange(true)) return;
  std::promise<void> done;
  auto f = done.get_future();
  asio::post(acceptor_.get_executor(), [this, &done] {
    std::error_code ec;
    acceptor_.close(ec);
    done.set_value();
  });
  f.wait_for(std::chrono::seconds(5));
}

void Listener::acceptNext() {
  acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
    if (stopped_ || !acceptor_.is_open()) return;
    if (!ec) on_accept_(std::make_shared<Connection>(std::move(socket)));
    acceptNext();
  });
}

JoinAddress parseJoinAddress(const std::string& text, uint16_t defaultPort) {
  JoinAddress a;
  a.port = defaultPort;
  std::string portText;
  if (text.empty()) throw std::invalid_argument("the address is empty");
  if (text.front() == '[') {
    const size_t close = text.find(']');
    if (close == std::string::npos) throw std::invalid_argument(fmt::format("'{}': missing ']'", text));
    a.host = text.substr(1, close - 1);
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') throw std::invalid_argument(fmt::format("'{}': expected ':' after ']'", text));
      portText = text.substr(close + 2);
    }
  } else if (std::count(text.begin(), text.end(), ':') == 1) {
    const size_t colon = text.find(':');
    a.host = text.substr(0, colon);
    portText = text.substr(colon + 1);
  } else {
    a.host = text;  // a name, an IPv4 address, or a bare IPv6 address
  }
  if (a.host.empty()) throw std::invalid_argument(fmt::format("'{}': no host", text));
  if (!portText.empty() || (text.back() == ':')) {
    unsigned v = 0;
    const auto [p, err] = std::from_chars(portText.data(), portText.data() + portText.size(), v);
    if (err != std::errc() || p != portText.data() + portText.size() || v == 0 || v > 65535)
      throw std::invalid_argument(fmt::format("'{}': the port must be 1-65535", text));
    a.port = static_cast<uint16_t>(v);
  }
  return a;
}

}  // namespace pen::net
