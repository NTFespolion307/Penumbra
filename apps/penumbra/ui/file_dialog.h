#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pen::ui {

struct FileFilter {
  std::string name;                     // "Scenes"
  std::vector<std::string> extensions;  // {"glb", "gltf", "obj", "pen.json"}
};

// The system's file dialogs (DECISIONS D-100): Windows' own, on Linux zenity or kdialog when installed. Return
// nothing when cancelled or when there is no dialog to show (the path fields can always be typed into).
std::optional<std::filesystem::path> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters);
std::optional<std::filesystem::path> saveFileDialog(const std::string& title, const std::filesystem::path& suggested,
                                                    const std::vector<FileFilter>& filters);
// Whether a dialog can be shown at all on this machine.
bool fileDialogsAvailable();

}  // namespace pen::ui
