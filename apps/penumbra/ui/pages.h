#pragma once

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "apps/penumbra/ui/model.h"
#include "apps/penumbra/ui/render_runner.h"
#include "apps/penumbra/ui/scene_info.h"
#include "apps/penumbra/ui/texture.h"
#include "core/settings/render_settings.h"

namespace pen::ui {

enum class Page { Dashboard, Hosts, HostDetail, Render, Viewport, Settings };

// The Render page: the chosen scene (loaded in the background), the settings being edited, the framing preview.
struct RenderPage {
  char scene_path[1024] = {};
  std::future<SceneInfo> loading;
  std::shared_ptr<SceneInfo> info;  // the last scene loaded
  RenderSettings form;              // what Start renders
  char output[1024] = {};
  int preset = -1;                  // index into resolutionPresets(), -1 = custom
  int custom_w = 1920, custom_h = 1080;
  std::future<std::vector<uint8_t>> preview;
  uint32_t preview_w = 0, preview_h = 0;
  bool preview_wanted = false;
  std::chrono::steady_clock::time_point changed{};  // the preview re-renders shortly after the framing changes
  ImageTexture preview_tex;
  std::vector<std::string> recent;  // recent scenes, newest first (user data folder)
  std::string start_error;
};

// What the pages share: the current page, choices made on them, and where the session listens.
struct UiState {
  Page page = Page::Dashboard;
  WorkerId host = 0;  // Host detail: which host (0 = this machine)
  RenderSettings settings;  // the defaults and command line; the Render page edits a copy
  std::string join_code;    // formatted XXXX-XXXX-XXXX-XXXX; empty when not hosting
  std::string hosting_error;  // why hosting could not start
  std::string worker_address;  // what to give workers, e.g. "192.168.1.20:7420" (refreshed every second)
  // Names given with Rename, and devices or hosts switched off (applied when a render starts).
  std::map<WorkerId, std::string> renamed;
  std::set<WorkerId> disabled_hosts;
  std::set<std::pair<WorkerId, uint32_t>> disabled_devices;
  RenderRunner* runner = nullptr;
  RenderPage render;
  std::string name(const HostRow& h) const {
    auto it = renamed.find(h.id);
    return it == renamed.end() ? h.name : it->second;
  }
};

void drawDashboard(Model& model, UiState& ui);
void drawHosts(Model& model, UiState& ui);
void drawHostDetail(Model& model, UiState& ui);
void drawRender(Model& model, UiState& ui);
// Loads the recent scenes list, and opens `scene` (from the command line) if given.
void initRenderPage(UiState& ui);

// Page header: title, subtitle and an optional right-aligned note.
void pageHeader(const char* title, const char* subtitle, const char* note = nullptr);

}  // namespace pen::ui
