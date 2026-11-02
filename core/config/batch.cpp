#include "core/config/batch.h"

#include <fmt/format.h>
#include <fstream>
#include <map>

#include "core/film/session.h"
#include "core/settings/parse.h"
#include "core/settings/schema.h"
#include "core/settings/serialize.h"
#include "core/system/file.h"
#include "core/system/paths.h"

namespace pen {

using nlohmann::json;

namespace {

uint64_t fnv1a(uint64_t h, std::string_view s) {
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// The same file whatever way the path is written (relative, "..", case on Windows).
std::string pathKey(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (ec) abs = p;
  const std::filesystem::path canon = std::filesystem::weakly_canonical(abs, ec);
  std::string key = pathToUtf8((ec ? abs : canon).lexically_normal());
#if defined(_WIN32)
  key = toLower(key);
#endif
  return key;
}

const char* sessionSettingDiffering(const RenderSettings& a, const RenderSettings& b) {
  if (a.port != b.port) return "port";
  if (a.bind != b.bind) return "bind";
  if (a.approve_workers != b.approve_workers) return "approve_workers";
  if (a.upload_limit != b.upload_limit) return "upload_limit";
  if (a.no_upnp != b.no_upnp) return "no_upnp";
  return nullptr;
}

}  // namespace

std::vector<BatchJob> loadBatch(const std::filesystem::path& file, const SettingsSources& base, bool hosting) {
  const std::string src = pathToUtf8(file);
  std::ifstream in(file, std::ios::binary);
  if (!in) throw SettingsError(1, fmt::format("--batch: cannot open '{}'", src));
  json j;
  try {
    j = json::parse(in, nullptr, true, /*ignore_comments=*/true);
  } catch (const json::parse_error& e) {
    throw SettingsError(1, fmt::format("--batch: '{}' is not valid JSON: {}", src, e.what()));
  }
  const json* list = &j;
  if (j.is_object()) {
    for (const auto& [key, value] : j.items())
      if (key != "jobs") throw SettingsError(1, fmt::format("{}: unknown key '{}' (a batch file has \"jobs\")", src, key));
    if (!j.contains("jobs")) throw SettingsError(1, fmt::format("{}: no \"jobs\" list", src));
    list = &j["jobs"];
  }
  if (!list->is_array() || list->empty())
    throw SettingsError(1, fmt::format("{}: \"jobs\" must be a list of one or more objects", src));

  const std::string fileName = pathToUtf8(file.filename());
  std::vector<BatchJob> jobs;
  for (size_t i = 0; i < list->size(); ++i) {
    const json& item = (*list)[i];
    const std::string label = fmt::format("{}, job {}", fileName, i + 1);
    if (!item.is_object()) throw SettingsError(1, fmt::format("{}: must be an object of settings", label));
    BatchJob job;
    job.name = fmt::format("job {}", i + 1);
    SettingsSources sources = base;
    sources.job = json::object();
    sources.job_label = label;
    for (const auto& [key, value] : item.items()) {
      if (key == "name") {
        if (!value.is_string() || value.get<std::string>().empty())
          throw SettingsError(1, fmt::format("{}: \"name\" must be a non-empty string", label));
        job.name = value.get<std::string>();
        continue;
      }
      // Strict, unlike the config file: a mistyped key would otherwise cost a whole render.
      if (!findSetting(key)) throw SettingsError(1, fmt::format("{}: unknown setting '{}'", label, key));
      sources.job[key] = value;
    }
    ResolvedSettings resolved = resolveSettings(sources);
    job.settings = std::move(resolved.settings);
    job.warnings = std::move(resolved.warnings);
    if (job.settings.scene.empty())
      throw SettingsError(1, fmt::format("{}: no scene (give \"scene\" in the job or a scene on the command line)", label));
    job.fingerprint = fnv1a(fnv1a(14695981039346656037ull, fmt::format("{:016x}", pixelSettingsHash(job.settings))),
                            pathKey(job.settings.output));
    jobs.push_back(std::move(job));
  }

  std::map<std::string, size_t> outputs, films;
  for (size_t i = 0; i < jobs.size(); ++i) {
    const auto unique = [&](std::map<std::string, size_t>& seen, const std::filesystem::path& p, const char* what) {
      const auto [it, inserted] = seen.emplace(pathKey(p), i);
      if (!inserted)
        throw SettingsError(1, fmt::format("{}: {} and {} would both write the {} '{}'; give each job its own output",
                                           fileName, jobs[it->second].name, jobs[i].name, what, pathToUtf8(p)));
    };
    unique(outputs, jobs[i].settings.output, "output");
    unique(films, filmPathFor(jobs[i].settings), "film");
  }
  if (hosting) {
    for (size_t i = 1; i < jobs.size(); ++i)
      if (const char* key = sessionSettingDiffering(jobs[0].settings, jobs[i].settings))
        throw SettingsError(1, fmt::format("{}: {} has a different '{}' than {}; the workers' session lasts the whole "
                                           "batch, so give it once (on the command line)",
                                           fileName, jobs[i].name, key, jobs[0].name));
  }
  return jobs;
}

std::filesystem::path batchStatePath(const std::filesystem::path& batchFile) {
  return batchFile.parent_path() / pathFromUtf8(pathToUtf8(batchFile.stem()) + ".batch-state.json");
}

std::set<uint64_t> loadBatchState(const std::filesystem::path& stateFile) {
  std::set<uint64_t> done;
  std::ifstream in(stateFile, std::ios::binary);
  if (!in) return done;
  try {
    const json j = json::parse(in);
    for (const auto& v : j.at("done")) done.insert(std::stoull(v.get<std::string>(), nullptr, 16));
  } catch (const std::exception&) {
    done.clear();  // a damaged state file only means finished jobs render again
  }
  return done;
}

void saveBatchState(const std::filesystem::path& stateFile, const std::set<uint64_t>& done) {
  json j = {{"done", json::array()}};
  for (const uint64_t f : done) j["done"].push_back(fmt::format("{:016x}", f));
  writeFileAtomically(stateFile, j.dump(2) + "\n");
}

}  // namespace pen
