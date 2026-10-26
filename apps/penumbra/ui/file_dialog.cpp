#include "apps/penumbra/ui/file_dialog.h"

#include <cstdio>
#include <cstdlib>

#include "core/system/paths.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace pen::ui {

namespace {

#if defined(_WIN32)

std::wstring widen(const std::string& s) { return pathFromUtf8(s).wstring(); }

std::optional<std::filesystem::path> showDialog(bool save, const std::string& title,
                                                const std::filesystem::path& suggested,
                                                const std::vector<FileFilter>& filters) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::optional<std::filesystem::path> result;
  IFileDialog* dlg = nullptr;
  const HRESULT made = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                        save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg));
  if (SUCCEEDED(made)) {
    // Filters: "Scenes (*.glb;*.gltf)" and "All files".
    std::vector<std::wstring> names, specs;
    for (const FileFilter& f : filters) {
      std::wstring spec;
      for (const std::string& e : f.extensions) spec += (spec.empty() ? L"" : L";") + std::wstring(L"*.") + widen(e);
      names.push_back(widen(f.name));
      specs.push_back(spec);
    }
    names.push_back(L"All files");
    specs.push_back(L"*.*");
    std::vector<COMDLG_FILTERSPEC> fs;
    for (size_t i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
    dlg->SetFileTypes(static_cast<UINT>(fs.size()), fs.data());
    dlg->SetTitle(widen(title).c_str());
    if (save && !suggested.empty()) {
      dlg->SetFileName(suggested.filename().wstring().c_str());
      if (!filters.empty() && !filters.front().extensions.empty())
        dlg->SetDefaultExtension(widen(filters.front().extensions.front()).c_str());
    }
    if (SUCCEEDED(dlg->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = std::filesystem::path(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

#else

bool have(const char* tool) {
  const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

std::string quote(const std::string& s) {
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

std::optional<std::filesystem::path> run(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return std::nullopt;
  std::string out;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
  const int rc = pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  if (rc != 0 || out.empty()) return std::nullopt;  // cancelled
  return pathFromUtf8(out);
}

std::optional<std::filesystem::path> showDialog(bool save, const std::string& title,
                                                const std::filesystem::path& suggested,
                                                const std::vector<FileFilter>& filters) {
  if (have("zenity")) {
    std::string cmd = "zenity --file-selection --title=" + quote(title);
    if (save) cmd += " --save --confirm-overwrite";
    if (!suggested.empty()) cmd += " --filename=" + quote(pathToUtf8(suggested));
    for (const FileFilter& f : filters) {
      std::string pat;
      for (const std::string& e : f.extensions) pat += " *." + e;
      cmd += " --file-filter=" + quote(f.name + " |" + pat);
    }
    return run(cmd + " 2>/dev/null");
  }
  if (have("kdialog")) {
    std::string pat;
    for (const FileFilter& f : filters)
      for (const std::string& e : f.extensions) pat += (pat.empty() ? "*." : " *.") + e;
    const std::string start = suggested.empty() ? std::string(".") : pathToUtf8(suggested);
    return run(std::string("kdialog --title ") + quote(title) + (save ? " --getsavefilename " : " --getopenfilename ") +
               quote(start) + " " + quote(pat) + " 2>/dev/null");
  }
  return std::nullopt;
}

#endif

}  // namespace

std::optional<std::filesystem::path> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters) {
  return showDialog(false, title, {}, filters);
}

std::optional<std::filesystem::path> saveFileDialog(const std::string& title, const std::filesystem::path& suggested,
                                                    const std::vector<FileFilter>& filters) {
  return showDialog(true, title, suggested, filters);
}

void openFolder(const std::filesystem::path& folder) {
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  const std::string cmd = "xdg-open " + quote(pathToUtf8(folder)) + " >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
#endif
}

std::string runSelf(const std::string& args) {
#if defined(_WIN32)
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::string cmd = "\"\"" + pathToUtf8(exe) + "\" " + args + " 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
#else
  const std::string cmd = quote(pathToUtf8(std::filesystem::read_symlink("/proc/self/exe"))) + " " + args + " 2>&1";
  FILE* p = popen(cmd.c_str(), "r");
#endif
  if (!p) return "could not run penumbra " + args;
  std::string out;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
#if defined(_WIN32)
  _pclose(p);
#else
  pclose(p);
#endif
  return out;
}

std::filesystem::path picturesFolder() {
#if defined(_WIN32)
  PWSTR dir = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &dir))) out = dir;
  CoTaskMemFree(dir);
  if (!out.empty()) return out;
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
  if (home && *home && std::filesystem::is_directory(pathFromUtf8(home) / "Pictures"))
    return pathFromUtf8(home) / "Pictures";
#endif
  return home && *home ? pathFromUtf8(home) : std::filesystem::temp_directory_path();
}

bool fileDialogsAvailable() {
#if defined(_WIN32)
  return true;
#else
  static const bool ok = have("zenity") || have("kdialog");
  return ok;
#endif
}

}  // namespace pen::ui
