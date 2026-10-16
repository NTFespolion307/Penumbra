#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pen {

// All strings in Penumbra are UTF-8; convert at the filesystem boundary with these.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& p);

namespace paths {

// Per-OS locations (docs/GUIDE.md). None of these create the folder.
std::filesystem::path userConfigFile();  // %APPDATA%\Penumbra\config.json | ~/.config/penumbra/config.json
std::filesystem::path logDir();          // %LOCALAPPDATA%\Penumbra\logs | ~/.local/state/penumbra/logs
std::filesystem::path sceneCacheDir();   // %LOCALAPPDATA%\Penumbra\cache\scenes | ~/.cache/penumbra/scenes
std::filesystem::path userDataDir();     // %LOCALAPPDATA%\Penumbra | ~/.local/share/penumbra (join code lives here)

}  // namespace paths
}  // namespace pen
