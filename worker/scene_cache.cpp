#include "worker/scene_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/settings/parse.h"
#include "core/system/hash.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"

namespace pen {

namespace fs = std::filesystem;

namespace {
std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Test-only (D-047 family): pretend the cache disk has this many bytes free.
uint64_t freeBytes(const fs::path& dir) {
  if (const char* fake = std::getenv("PENUMBRA_TEST_FREE_DISK")) {
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_FREE_DISK): pretending {} bytes are free", fake);
    return std::strtoull(fake, nullptr, 10);
  }
  return freeDiskBytes(dir);
}
}  // namespace

SceneCache::SceneCache(fs::path dir, uint64_t limitBytes) : dir_(std::move(dir)), limit_(limitBytes) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
}

fs::path SceneCache::folder(uint64_t hash) const { return dir_ / toHex(hash); }

void SceneCache::touch(const fs::path& f) const {
  std::error_code ec;
  fs::last_write_time(f / "manifest.json", fs::file_time_type::clock::now(), ec);
}

std::optional<fs::path> SceneCache::find(const ScenePackage& pkg, uint64_t hash) {
  const fs::path f = folder(hash);
  std::error_code ec;
  if (!fs::is_regular_file(f / "manifest.json", ec)) return std::nullopt;
  if (readText(f / "manifest.json") != pkg.manifest()) return std::nullopt;
  touch(f);
  return f;
}

std::optional<std::string> SceneCache::cannotHold(const ScenePackage& pkg) const {
  if (pkg.total_bytes > limit_)
    return fmt::format("the scene ({}) is bigger than this worker's scene cache (--cache-size {})",
                       formatBytes(pkg.total_bytes), formatBytes(limit_));
  // The download and the unpacked copy exist side by side for a moment.
  const uint64_t need = pkg.total_bytes * 2, free = freeBytes(dir_);
  if (free < need)
    return fmt::format("not enough disk for the scene in '{}': it needs {}, {} is free (use --cache-dir on a bigger "
                       "drive)",
                       pathToUtf8(dir_), formatBytes(need), formatBytes(free));
  const SystemInfo sys = querySystem();
  if (sys.ram_available > 0 && pkg.total_bytes > sys.ram_available)
    return fmt::format("not enough RAM for the scene: its files alone are {}, {} of RAM is available",
                       formatBytes(pkg.total_bytes), formatBytes(sys.ram_available));
  return std::nullopt;
}

uint32_t SceneCache::beginDownload(const ScenePackage& pkg, uint64_t hash) {
  pkg_ = pkg;
  hash_ = hash;
  downloading_ = true;
  next_ = 0;
  const fs::path path = dir_ / (toHex(hash) + ".partial");
  std::error_code ec;
  const bool resume = fs::exists(path, ec);
  // A worker that just crashed may still hold the file for a moment (Windows closes handles after the process
  // is gone), so a busy file is retried for a few seconds.
  for (int attempt = 0;; ++attempt) {
    try {
      partial_ = BinaryFile(path, resume ? BinaryFile::Mode::ReadWrite : BinaryFile::Mode::Create);
      break;
    } catch (const FileError&) {
      if (attempt >= 50) throw;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (resume) {
    // Keep the chunks that are complete and still match their hashes.
    const uint64_t have = partial_.size();
    std::vector<uint8_t> buf;
    while (next_ < pkg_.chunkCount()) {
      const uint64_t off = uint64_t(next_) * ScenePackage::kChunkBytes;
      const uint32_t n = pkg_.chunkSize(next_);
      if (off + n > have) break;
      buf.resize(n);
      partial_.readAt(off, buf.data(), n);
      if (hash64(buf) != pkg_.chunk_hashes[next_]) break;
      ++next_;
    }
    partial_.truncate(uint64_t(next_) * ScenePackage::kChunkBytes);
  }
  return next_;
}

void SceneCache::addChunk(uint32_t index, std::span<const uint8_t> data) {
  if (!downloading_ || index != next_ || index >= pkg_.chunkCount())
    throw FileError(fmt::format("scene chunk {} arrived out of order (expected {})", index, next_));
  if (data.size() != pkg_.chunkSize(index) || hash64(data) != pkg_.chunk_hashes[index])
    throw FileError(fmt::format("scene chunk {} does not match its hash", index));
  partial_.writeAt(uint64_t(index) * ScenePackage::kChunkBytes, data.data(), data.size());
  ++next_;
}

fs::path SceneCache::finishDownload() {
  if (!downloadComplete()) throw FileError("the scene download is not complete");
  const fs::path tmp = dir_ / (toHex(hash_) + ".tmp"), final = folder(hash_);
  std::error_code ec;
  fs::remove_all(tmp, ec);
  uint64_t offset = 0;
  std::vector<uint8_t> buf;
  for (const PackageFile& f : pkg_.files) {
    const fs::path out = tmp / pathFromUtf8(f.path);
    fs::create_directories(out.parent_path());
    BinaryFile file(out, BinaryFile::Mode::Create);
    Hasher h;
    uint64_t done = 0;
    while (done < f.size) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(f.size - done, 1u << 22));
      buf.resize(n);
      partial_.readAt(offset + done, buf.data(), n);
      h.update(buf);
      file.writeAt(done, buf.data(), n);
      done += n;
    }
    file.close();
    if (h.finish() != f.hash) throw FileError(fmt::format("'{}' in the downloaded scene does not match its hash", f.path));
    offset += f.size;
  }
  writeFileAtomically(tmp / "manifest.json", pkg_.manifest());
  fs::remove_all(final, ec);
  fs::rename(tmp, final);
  partial_.close();
  fs::remove(dir_ / (toHex(hash_) + ".partial"), ec);
  downloading_ = false;
  touch(final);
  evict(final);
  return final;
}

SceneCache::Usage SceneCache::usage() const {
  Usage u;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(dir_, ec)) {
    const fs::path m = d.path() / "manifest.json";
    if (!d.is_directory() || !fs::is_regular_file(m, ec)) continue;
    try {
      u.bytes += ScenePackage::fromManifest(readText(m)).total_bytes;
      ++u.scenes;
    } catch (const std::exception&) {
    }
  }
  return u;
}

void SceneCache::evict(const fs::path& keep) {
  struct Entry {
    fs::path path;
    fs::file_time_type used;
    uint64_t bytes;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(dir_, ec)) {
    const fs::path m = d.path() / "manifest.json";
    if (!d.is_directory() || !fs::is_regular_file(m, ec)) continue;
    uint64_t bytes = 0;
    try {
      bytes = ScenePackage::fromManifest(readText(m)).total_bytes;
    } catch (const std::exception&) {
      continue;
    }
    entries.push_back({d.path(), fs::last_write_time(m, ec), bytes});
    total += bytes;
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
  for (const Entry& e : entries) {
    if (total <= limit_) break;
    if (e.path == keep) continue;
    spdlog::info("scene cache over {}: removing {} ({})", formatBytes(limit_), pathToUtf8(e.path.filename()),
                 formatBytes(e.bytes));
    fs::remove_all(e.path, ec);
    total -= e.bytes;
  }
}

}  // namespace pen
