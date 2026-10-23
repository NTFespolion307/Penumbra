#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/package/package.h"
#include "core/system/file.h"

namespace pen {

// The worker's scene cache (docs/GUIDE.md "Worker cache"): one folder per package hash holding the unpacked
// files and the manifest. A download in progress is a `<hash>.partial` file holding the verified chunks so far,
// so an interrupted download resumes at the first missing chunk. Least-recently-used packages are evicted when
// the cache grows over its size limit.
class SceneCache {
 public:
  SceneCache(std::filesystem::path dir, uint64_t limitBytes);

  const std::filesystem::path& dir() const { return dir_; }
  // The unpacked package folder when the package is complete in the cache (and marks it as just used).
  std::optional<std::filesystem::path> find(const ScenePackage& pkg, uint64_t hash);

  // Starts or resumes a download; returns the first chunk still missing.
  uint32_t beginDownload(const ScenePackage& pkg, uint64_t hash);
  // Stores the next chunk; throws FileError when it does not match its hash or does not come next.
  void addChunk(uint32_t index, std::span<const uint8_t> data);
  bool downloadComplete() const { return downloading_ && next_ == pkg_.chunkCount(); }
  // Unpacks the finished download into the cache, verifies every file, evicts old packages. Returns the folder.
  std::filesystem::path finishDownload();

  struct Usage {
    uint64_t bytes = 0;  // complete packages
    uint32_t scenes = 0;
  };
  Usage usage() const;
  uint64_t limit() const { return limit_; }

  // Bytes the cache may still take for this package (free disk, limit). For scene_rejected.
  std::optional<std::string> cannotHold(const ScenePackage& pkg) const;

 private:
  std::filesystem::path folder(uint64_t hash) const;
  void touch(const std::filesystem::path& folder) const;
  void evict(const std::filesystem::path& keep);

  std::filesystem::path dir_;
  uint64_t limit_;
  bool downloading_ = false;
  ScenePackage pkg_;
  uint64_t hash_ = 0;
  uint32_t next_ = 0;
  BinaryFile partial_;
};

}  // namespace pen
