#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/package/package.h"
#include "net/connection.h"

namespace pen {

// Sends scene packages to workers (docs/GUIDE.md "Transfer"): chunks in order, compressed, to several workers
// at once (round-robin), never more than 2 chunks queued per connection so control messages and tile acks keep
// flowing, and at most --upload-limit bytes per second in total.
class SceneUploader {
 public:
  explicit SceneUploader(uint64_t limitBytesPerSecond);
  ~SceneUploader();

  // Sends chunks first..end of `pkg` on `connection`, replacing any transfer already going to it.
  void start(std::shared_ptr<net::Connection> connection, std::shared_ptr<const ScenePackage> pkg, uint64_t hash,
             uint32_t first, const std::string& workerName);
  void stop();

 private:
  struct Transfer {
    std::shared_ptr<net::Connection> connection;
    std::shared_ptr<const ScenePackage> pkg;
    uint64_t hash = 0;
    uint32_t next = 0;
    std::string name;
    uint64_t bytes_sent = 0;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point progressed;  // last chunk handed to the connection
    bool stall_logged = false;
  };
  void run();

  uint64_t limit_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Transfer> transfers_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace pen
