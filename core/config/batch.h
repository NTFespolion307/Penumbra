#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/config/config.h"

namespace pen {

// One render of a --batch file.
struct BatchJob {
  std::string name;  // "name" from the file, else "job N"
  RenderSettings settings;
  std::vector<std::string> warnings;  // from resolving its settings; log them once logging is up
  uint64_t fingerprint = 0;  // pixel settings + output path: a finished job is skipped when the batch runs again
};

// Reads a --batch file: {"jobs": [{...}, ...]}, or just the list. A job holds setting keys as in the config file and
// the .pen.json "render" section, plus "scene" and an optional "name". Its settings win over the command line
// (`base.cli`), which wins over the .pen.json, the config file and the defaults; a scene on the command line is the
// default for jobs without one. Paths are relative to the current folder, as on the command line.
// Throws SettingsError (exit code 1, or 2 for scene problems) for a bad file or job, an unknown key, a job without a
// scene, two jobs writing the same output or film, and, when `hosting`, jobs that disagree on a setting of the
// workers' session (port, bind, approve_workers, upload_limit, no_upnp), which lasts the whole batch.
std::vector<BatchJob> loadBatch(const std::filesystem::path& file, const SettingsSources& base, bool hosting);

// The finished jobs of a batch, by fingerprint, in <batch file stem>.batch-state.json next to the batch file: running
// the same batch again skips them. Removed once every job has finished.
std::filesystem::path batchStatePath(const std::filesystem::path& batchFile);
std::set<uint64_t> loadBatchState(const std::filesystem::path& stateFile);  // empty when missing or unreadable
void saveBatchState(const std::filesystem::path& stateFile, const std::set<uint64_t>& done);  // throws FileError

}  // namespace pen
