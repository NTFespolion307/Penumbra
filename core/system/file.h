#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace pen {

struct FileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// Writing failed because the disk is full (exit code 4).
struct DiskFullError : FileError {
  using FileError::FileError;
};

// A binary file with positioned reads and writes (files larger than 4 GB, both OSes) and durable flushes.
class BinaryFile {
 public:
  enum class Mode { Read, ReadWrite, Create };  // Create truncates or creates

  BinaryFile() = default;
  BinaryFile(const std::filesystem::path& path, Mode mode);
  ~BinaryFile();
  BinaryFile(BinaryFile&& other) noexcept;
  BinaryFile& operator=(BinaryFile&& other) noexcept;
  BinaryFile(const BinaryFile&) = delete;
  BinaryFile& operator=(const BinaryFile&) = delete;

  bool isOpen() const;
  void readAt(uint64_t offset, void* data, size_t size) const;
  void writeAt(uint64_t offset, const void* data, size_t size);
  uint64_t size() const;
  void truncate(uint64_t size);
  void sync();  // data and metadata reach the disk (fsync / FlushFileBuffers)
  void close();
  const std::filesystem::path& path() const { return path_; }

 private:
  [[noreturn]] void fail(const char* what) const;
  std::filesystem::path path_;
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

// Writes a whole file atomically: temp file in the same folder, flushed, then renamed over the target.
void writeFileAtomically(const std::filesystem::path& path, const std::string& content);

// Free bytes on the volume holding `path` (or its nearest existing parent).
uint64_t freeDiskBytes(std::filesystem::path path);

}  // namespace pen
