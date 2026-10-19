#pragma once

// Scene packages (docs/GUIDE.md "Scene packages and distribution"): every file a scene needs, described by a
// manifest with sizes and BLAKE3 hashes. Workers download a package once and keep it in their cache; the
// package hash is the hash of the manifest, so the same files always give the same package.
//
// On the wire a package is the concatenation of its files in manifest order, cut into 4 MB chunks; each chunk's
// hash is in the manifest, so every chunk is checked on arrival and an interrupted download resumes at the first
// missing chunk. Render settings are not part of the package (they travel with each render), so re-rendering
// the same scene with other settings reuses the cache (DECISIONS D-062).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/settings/render_settings.h"

namespace pen {

struct PackageFile {
  std::string path;  // relative to the package root, '/'-separated
  uint64_t size = 0;
  uint64_t hash = 0;  // BLAKE3-64 of the file
};

struct ScenePackage {
  static constexpr uint32_t kChunkBytes = 4u << 20;

  std::string app_version;
  std::string entry;  // the scene file (relative path)
  std::string hdri;   // the environment map (relative path), or empty
  std::vector<PackageFile> files;
  std::vector<uint64_t> chunk_hashes;
  uint64_t total_bytes = 0;

  // Controller side only: where the files are on this machine (same order as `files`).
  std::vector<std::filesystem::path> sources;

  uint32_t chunkCount() const { return static_cast<uint32_t>(chunk_hashes.size()); }
  uint32_t chunkSize(uint32_t index) const;
  std::string manifest() const;  // canonical JSON
  uint64_t hash() const;         // BLAKE3-64 of manifest()
  // Parses a manifest; throws SceneError when it is malformed or names unsafe paths ("..", absolute).
  static ScenePackage fromManifest(const std::string& json);

  // The settings with the scene and HDRI paths pointing into a package unpacked at `root`.
  RenderSettings relocate(const RenderSettings& settings, const std::filesystem::path& root) const;
};

// Every file the scene in `settings` reads: the scene file, glTF buffers and images, OBJ material libraries and
// textures, the HDRI. Throws SceneError naming the first missing file.
std::vector<std::filesystem::path> collectSceneFiles(const RenderSettings& settings);

// Collects, hashes and describes the scene's files (the validation step of every render, also solo).
// Throws SceneError.
ScenePackage buildPackage(const RenderSettings& settings);

// Controller: the bytes of chunk `index`, read from the source files. Throws FileError.
std::vector<uint8_t> readPackageChunk(const ScenePackage& pkg, uint32_t index);

}  // namespace pen
