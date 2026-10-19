// penumbra-netem-proxy: a TCP proxy that makes a bad network for the network tests (docs/GUIDE.md "Network
// tests": 200 ms latency, 2% packet loss, a 10 Mbit/s uplink).
//
//   penumbra-netem-proxy --listen PORT --to HOST:PORT [--delay-ms 100] [--loss 0.02] [--up-kbps 10000]
//                        [--down-kbps 0]
//
// Every block of data is held back --delay-ms in each direction (so 100 gives a 200 ms round trip). A user-space
// proxy cannot drop TCP segments, so --loss is the chance that a block is delayed a further 200 ms, the time
// TCP takes to retransmit a lost segment; blocks behind it wait too, as they would. "up" is the direction from
// the connecting side (the worker) to the target (the controller); 0 kbps = unlimited.

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  uint16_t listen = 0;
  std::string host;
  std::string port;
  int delay_ms = 100;
  double loss = 0.02;
  int up_kbps = 10000;
  int down_kbps = 0;
};

struct Block {
  std::vector<uint8_t> data;
  Clock::time_point due;
};

// One direction of one connection: a reader queues blocks with their due time, a writer sends them in order.
class Pump {
 public:
  Pump(tcp::socket& from, tcp::socket& to, int delayMs, double loss, int kbps, uint32_t seed)
      : from_(from), to_(to), delay_(delayMs), loss_(loss), kbps_(kbps), rng_(seed) {}

  void run() {
    std::thread writer([this] { write(); });
    std::vector<uint8_t> buf(16384);
    for (;;) {
      std::error_code ec;
      const size_t n = from_.read_some(asio::buffer(buf), ec);
      if (ec || n == 0) break;
      auto due = Clock::now() + std::chrono::milliseconds(delay_);
      if (std::uniform_real_distribution<double>(0, 1)(rng_) < loss_) due += std::chrono::milliseconds(200);
      std::lock_guard lock(mutex_);
      queue_.push_back({std::vector<uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)), due});
      cv_.notify_one();
    }
    {
      std::lock_guard lock(mutex_);
      done_ = true;
      cv_.notify_one();
    }
    writer.join();
    std::error_code ec;
    to_.shutdown(tcp::socket::shutdown_send, ec);
  }

 private:
  void write() {
    Clock::time_point lastDue{};
    for (;;) {
      Block b;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return done_ || !queue_.empty(); });
        if (queue_.empty()) return;
        b = std::move(queue_.front());
        queue_.pop_front();
      }
      b.due = std::max(b.due, lastDue);  // in order: a stalled block holds back the ones behind it
      lastDue = b.due;
      std::this_thread::sleep_until(b.due);
      std::error_code ec;
      asio::write(to_, asio::buffer(b.data), ec);
      if (ec) return;
      if (kbps_ > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(int64_t(b.data.size()) * 8 * 1000 / kbps_));
    }
  }

  tcp::socket& from_;
  tcp::socket& to_;
  int delay_;
  double loss_;
  int kbps_;
  std::mt19937 rng_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Block> queue_;
  bool done_ = false;
};

void serve(std::shared_ptr<tcp::socket> client, const Options& o, uint32_t seed) {
  asio::io_context io;
  tcp::socket server(io);
  std::error_code ec;
  tcp::resolver resolver(io);
  asio::connect(server, resolver.resolve(o.host, o.port, ec), ec);
  if (ec) {
    std::fprintf(stderr, "netem-proxy: cannot reach %s:%s: %s\n", o.host.c_str(), o.port.c_str(), ec.message().c_str());
    return;
  }
  server.set_option(tcp::no_delay(true), ec);
  client->set_option(tcp::no_delay(true), ec);
  Pump up(*client, server, o.delay_ms, o.loss, o.up_kbps, seed);
  Pump down(server, *client, o.delay_ms, o.loss, o.down_kbps, seed + 1);
  std::thread t([&] { up.run(); });
  down.run();
  t.join();
  std::printf("netem-proxy: connection closed\n");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--listen") o.listen = static_cast<uint16_t>(std::atoi(v.c_str()));
    else if (k == "--to") {
      const size_t colon = v.rfind(':');
      o.host = v.substr(0, colon);
      o.port = v.substr(colon + 1);
    } else if (k == "--delay-ms") o.delay_ms = std::atoi(v.c_str());
    else if (k == "--loss") o.loss = std::atof(v.c_str());
    else if (k == "--up-kbps") o.up_kbps = std::atoi(v.c_str());
    else if (k == "--down-kbps") o.down_kbps = std::atoi(v.c_str());
    else {
      std::fprintf(stderr, "netem-proxy: unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (o.listen == 0 || o.host.empty()) {
    std::fprintf(stderr, "usage: penumbra-netem-proxy --listen PORT --to HOST:PORT [--delay-ms 100] [--loss 0.02] "
                         "[--up-kbps 10000] [--down-kbps 0]\n");
    return 2;
  }
  asio::io_context io;
  tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), o.listen));
  std::printf("netem-proxy: 127.0.0.1:%u -> %s:%s, %d ms each way, %.1f%% stalls, up %d kbit/s, down %d kbit/s\n",
              o.listen, o.host.c_str(), o.port.c_str(), o.delay_ms, o.loss * 100, o.up_kbps, o.down_kbps);
  std::fflush(stdout);
  uint32_t seed = 1;
  for (;;) {
    auto client = std::make_shared<tcp::socket>(io);
    std::error_code ec;
    acceptor.accept(*client, ec);
    if (ec) continue;
    std::thread([client, o, s = seed] { serve(client, o, s); }).detach();
    seed += 2;
  }
}
