#include "core/film/session.h"

#include <ctime>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/settings/schema.h"
#include "core/settings/serialize.h"
#include "core/system/file.h"
#include "core/system/hash.h"
#include "core/system/paths.h"
#include "core/version.h"

namespace pen {

using nlohmann::json;

std::string localTimestamp() {
  const std::time_t now = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  return buf;
}

std::filesystem::path filmPathFor(const RenderSettings& s) {
  const std::filesystem::path dir = s.film_dir.empty() ? s.output.parent_path() : s.film_dir;
  std::filesystem::path name = s.output.stem();
  name += ".penfilm";
  return dir / name;
}

std::filesystem::path sessionPathFor(const std::filesystem::path& filmPath) {
  std::filesystem::path p = filmPath;
  p.replace_extension(".session.json");
  return p;
}

uint64_t sceneFingerprint(const RenderSettings& s) {
  std::string parts;
  for (const auto* p : {&s.scene, &s.scene_description, &s.environment.hdri})
    parts += p->empty() ? std::string("-") : toHex(hashFile(*p));
  return hash64(parts);
}

void Session::save(const std::filesystem::path& path) const {
  json j;
  j["penumbra_session"] = 1;
  j["app_version"] = app_version;
  j["git_hash"] = git_hash;
  j["created"] = created;
  j["updated"] = updated;
  j["state"] = state;
  j["film"] = pathToUtf8(std::filesystem::absolute(film));
  j["scene_hash"] = toHex(scene_hash);
  j["settings_hash"] = toHex(settings_hash);
  j["progress"] = {{"tiles_done", tiles_done}, {"tiles_total", tiles_total}, {"samples_done", samples_done},
                   {"samples_total", samples_total}};
  j["settings"] = settingsToJson(settings);
  if (hosting) j["hosting"] = {{"join_code", join_code}};
  writeFileAtomically(path, j.dump(2) + "\n");
}

Session Session::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw SettingsError(1, fmt::format("cannot open session file '{}'", pathToUtf8(path)));
  try {
    const json j = json::parse(in);
    if (!j.contains("penumbra_session")) throw SettingsError(1, fmt::format("'{}' is not a Penumbra session file", pathToUtf8(path)));
    Session s;
    s.app_version = j.at("app_version").get<std::string>();
    s.git_hash = j.at("git_hash").get<std::string>();
    s.created = j.at("created").get<std::string>();
    s.updated = j.at("updated").get<std::string>();
    s.state = j.at("state").get<std::string>();
    s.film = pathFromUtf8(j.at("film").get<std::string>());
    s.scene_hash = fromHex(j.at("scene_hash").get<std::string>());
    s.settings_hash = fromHex(j.at("settings_hash").get<std::string>());
    const json& p = j.at("progress");
    s.tiles_done = p.at("tiles_done").get<uint32_t>();
    s.tiles_total = p.at("tiles_total").get<uint32_t>();
    s.samples_done = p.at("samples_done").get<uint64_t>();
    s.samples_total = p.at("samples_total").get<uint64_t>();
    s.settings = settingsFromJson(j.at("settings"));
    if (j.contains("hosting")) {
      s.hosting = true;
      s.join_code = j.at("hosting").value("join_code", std::string());
    }
    return s;
  } catch (const json::exception& e) {
    throw SettingsError(1, fmt::format("session file '{}' is damaged: {}", pathToUtf8(path), e.what()));
  }
}

}  // namespace pen
