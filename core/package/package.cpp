#include "core/package/package.h"

#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

#include "core/scene/scene_loader.h"
#include "core/settings/parse.h"
#include "core/system/file.h"
#include "core/system/hash.h"
#include "core/system/paths.h"
#include "core/version.h"

namespace pen {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string decodedUri(const char* uri) {
  std::string u = uri;
  cgltf_decode_uri(u.data());
  u.resize(std::strlen(u.c_str()));
  return u;
}

bool isDataUri(const char* uri) { return std::strncmp(uri, "data:", 5) == 0; }

void requireFile(const fs::path& p, const std::string& what, const fs::path& referencedBy) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec))
    throw SceneError(fmt::format("{} '{}' (used by '{}') is missing", what, pathToUtf8(p), pathToUtf8(referencedBy.filename())));
}

void collectGltf(const fs::path& path, std::vector<fs::path>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw SceneError(fmt::format("cannot open scene '{}'", pathToUtf8(path)));
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success)
    throw SceneError(fmt::format("'{}' is not a valid glTF file", pathToUtf8(path)));
  const fs::path base = path.parent_path();
  for (size_t i = 0; i < data->buffers_count; ++i) {
    const char* uri = data->buffers[i].uri;
    if (!uri || isDataUri(uri)) continue;  // GLB binary chunk or embedded
    const fs::path p = base / pathFromUtf8(decodedUri(uri));
    requireFile(p, "glTF buffer", path);
    out.push_back(p);
  }
  for (size_t i = 0; i < data->images_count; ++i) {
    const char* uri = data->images[i].uri;
    if (!uri || isDataUri(uri)) continue;  // in a buffer view or embedded
    const fs::path p = base / pathFromUtf8(decodedUri(uri));
    requireFile(p, "texture", path);
    out.push_back(p);
  }
  cgltf_free(data);
}

// The last whitespace-separated token: OBJ/MTL map statements put options before the file name.
std::string lastToken(const std::string& line) {
  std::istringstream ss(line);
  std::string t, last;
  while (ss >> t) last = t;
  return last;
}

void collectObj(const fs::path& path, std::vector<fs::path>& out) {
  std::ifstream in(path);
  if (!in) throw SceneError(fmt::format("cannot open scene '{}'", pathToUtf8(path)));
  const fs::path base = path.parent_path();
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("mtllib", 0) != 0) continue;
    std::istringstream ss(line.substr(6));
    std::string name;
    while (ss >> name) {
      const fs::path mtl = base / pathFromUtf8(name);
      requireFile(mtl, "material library", path);
      out.push_back(mtl);
      std::ifstream m(mtl);
      std::string ml;
      while (std::getline(m, ml)) {
        const std::string t = std::string(trim(ml));
        if (t.rfind("map_Kd", 0) != 0) continue;  // the only map the OBJ loader reads
        const fs::path tex = base / pathFromUtf8(lastToken(t));
        requireFile(tex, "texture", mtl);
        out.push_back(tex);
      }
    }
  }
}

fs::path absoluteNormal(const fs::path& p) { return fs::absolute(p).lexically_normal(); }

// The deepest folder that contains every file.
fs::path commonRoot(const std::vector<fs::path>& files) {
  fs::path root = files.front().parent_path();
  for (const auto& f : files) {
    if (f.root_name() != root.root_name())
      throw SceneError(fmt::format("the scene's files are on different drives ('{}' and '{}'); put them in one folder "
                                   "tree so they can be packaged",
                                   pathToUtf8(files.front()), pathToUtf8(f)));
    while (!root.empty()) {
      const fs::path rel = f.lexically_relative(root);
      if (!rel.empty() && *rel.begin() != "..") break;
      if (root == root.parent_path()) break;
      root = root.parent_path();
    }
  }
  return root;
}

bool safeRelative(const std::string& p) {
  if (p.empty() || p.front() == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
    return false;
  size_t start = 0;
  for (;;) {
    const size_t slash = p.find('/', start);
    const std::string part = p.substr(start, slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

}  // namespace

uint32_t ScenePackage::chunkSize(uint32_t index) const {
  const uint64_t begin = uint64_t(index) * kChunkBytes;
  return static_cast<uint32_t>(std::min<uint64_t>(kChunkBytes, total_bytes - begin));
}

std::string ScenePackage::manifest() const {
  json j;
  j["penumbra_package"] = 1;
  j["app_version"] = app_version;
  j["entry"] = entry;
  j["hdri"] = hdri;
  j["chunk_bytes"] = kChunkBytes;
  j["total_bytes"] = total_bytes;
  json list = json::array();
  for (const auto& f : files) list.push_back({{"path", f.path}, {"size", f.size}, {"hash", toHex(f.hash)}});
  j["files"] = list;
  json chunks = json::array();
  for (uint64_t h : chunk_hashes) chunks.push_back(toHex(h));
  j["chunks"] = chunks;
  return j.dump();
}

uint64_t ScenePackage::hash() const { return hash64(manifest()); }

ScenePackage ScenePackage::fromManifest(const std::string& text) {
  ScenePackage p;
  try {
    const json j = json::parse(text);
    if (j.at("penumbra_package").get<int>() != 1) throw SceneError("unknown package format");
    if (j.at("chunk_bytes").get<uint32_t>() != kChunkBytes) throw SceneError("unexpected package chunk size");
    p.app_version = j.at("app_version").get<std::string>();
    p.entry = j.at("entry").get<std::string>();
    p.hdri = j.at("hdri").get<std::string>();
    p.total_bytes = j.at("total_bytes").get<uint64_t>();
    uint64_t sum = 0;
    for (const auto& f : j.at("files")) {
      PackageFile pf{f.at("path").get<std::string>(), f.at("size").get<uint64_t>(),
                     fromHex(f.at("hash").get<std::string>())};
      if (!safeRelative(pf.path)) throw SceneError(fmt::format("unsafe path '{}' in the package", pf.path));
      sum += pf.size;
      p.files.push_back(std::move(pf));
    }
    for (const auto& c : j.at("chunks")) p.chunk_hashes.push_back(fromHex(c.get<std::string>()));
    const uint64_t chunks = (p.total_bytes + kChunkBytes - 1) / kChunkBytes;
    if (sum != p.total_bytes || p.chunk_hashes.size() != chunks) throw SceneError("inconsistent package manifest");
    const auto listed = [&](const std::string& path) {
      return std::any_of(p.files.begin(), p.files.end(), [&](const PackageFile& f) { return f.path == path; });
    };
    if (!listed(p.entry) || (!p.hdri.empty() && !listed(p.hdri)))
      throw SceneError("the package manifest names a scene file it does not contain");
  } catch (const json::exception& e) {
    throw SceneError(fmt::format("malformed package manifest: {}", e.what()));
  }
  return p;
}

RenderSettings ScenePackage::relocate(const RenderSettings& settings, const fs::path& root) const {
  RenderSettings s = settings;
  s.scene = root / pathFromUtf8(entry);
  s.environment.hdri = hdri.empty() ? fs::path() : root / pathFromUtf8(hdri);
  s.scene_description.clear();
  return s;
}

std::vector<fs::path> collectSceneFiles(const RenderSettings& s) {
  if (s.scene.empty()) throw SceneError("no scene given");
  const fs::path scene = absoluteNormal(s.scene);
  std::error_code ec;
  if (!fs::is_regular_file(scene, ec)) throw SceneError(fmt::format("scene file not found: {}", pathToUtf8(s.scene)));
  std::vector<fs::path> files{scene};
  const std::string ext = toLower(pathToUtf8(scene.extension()));
  if (ext == ".obj") collectObj(scene, files);
  else collectGltf(scene, files);
  if (!s.environment.hdri.empty()) {
    const fs::path hdri = absoluteNormal(s.environment.hdri);
    if (!fs::is_regular_file(hdri, ec))
      throw SceneError(fmt::format("environment map '{}' is missing", pathToUtf8(s.environment.hdri)));
    files.push_back(hdri);
  }
  // Absolute, normalised and without duplicates, in first-use order.
  std::vector<fs::path> unique;
  std::set<fs::path> seen;
  for (auto& f : files) {
    fs::path n = absoluteNormal(f);
    if (seen.insert(n).second) unique.push_back(std::move(n));
  }
  return unique;
}

ScenePackage buildPackage(const RenderSettings& settings) {
  const std::vector<fs::path> files = collectSceneFiles(settings);
  const fs::path root = commonRoot(files);
  ScenePackage pkg;
  pkg.app_version = std::string(appVersion());
  const auto rel = [&](const fs::path& p) { return p.lexically_relative(root).generic_u8string(); };
  const auto toStr = [](const auto& u8) { return std::string(u8.begin(), u8.end()); };
  pkg.entry = toStr(rel(files.front()));
  if (!settings.environment.hdri.empty()) pkg.hdri = toStr(rel(absoluteNormal(settings.environment.hdri)));

  // One pass over the concatenated files: per-file hashes and per-chunk hashes.
  std::vector<uint8_t> buf(1 << 20);
  Hasher chunk;
  uint64_t inChunk = 0;
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    if (!in) throw SceneError(fmt::format("cannot read '{}'", pathToUtf8(f)));
    Hasher file;
    uint64_t size = 0;
    while (in) {
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
      size_t n = static_cast<size_t>(in.gcount());
      file.update(std::span(buf.data(), n));
      size += n;
      size_t off = 0;
      while (n > 0) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(n, ScenePackage::kChunkBytes - inChunk));
        chunk.update(std::span(buf.data() + off, take));
        inChunk += take;
        off += take;
        n -= take;
        if (inChunk == ScenePackage::kChunkBytes) {
          pkg.chunk_hashes.push_back(chunk.finish());
          chunk = Hasher();
          inChunk = 0;
        }
      }
    }
    pkg.files.push_back({toStr(rel(f)), size, file.finish()});
    pkg.sources.push_back(f);
    pkg.total_bytes += size;
  }
  if (inChunk > 0) pkg.chunk_hashes.push_back(chunk.finish());
  return pkg;
}

std::vector<uint8_t> readPackageChunk(const ScenePackage& pkg, uint32_t index) {
  if (index >= pkg.chunkCount()) throw FileError(fmt::format("package chunk {} does not exist", index));
  const uint64_t begin = uint64_t(index) * ScenePackage::kChunkBytes;
  std::vector<uint8_t> out(pkg.chunkSize(index));
  uint64_t fileStart = 0;
  size_t written = 0;
  for (size_t i = 0; i < pkg.files.size() && written < out.size(); ++i) {
    const uint64_t fileEnd = fileStart + pkg.files[i].size;
    const uint64_t from = std::max(begin + written, fileStart);
    if (from < fileEnd) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(fileEnd - from, out.size() - written));
      BinaryFile f(pkg.sources[i], BinaryFile::Mode::Read);
      f.readAt(from - fileStart, out.data() + written, n);
      written += n;
    }
    fileStart = fileEnd;
  }
  if (written != out.size()) throw FileError("the scene files changed size while being sent");
  return out;
}

}  // namespace pen
