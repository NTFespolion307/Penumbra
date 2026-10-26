#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
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
#include "worker/client/worker_client.h"

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
  bool from_command_line = false;  // the loading scene came with the command line: keep its settings (--spp, --res...)
};

// The Settings page's hosting switches and the defaults being edited.
struct SettingsPage {
  bool lan = true;       // LAN hosting starts with the app (docs/GUIDE.md "Render lifecycle", step 2)
  bool internet = false;  // UPnP; opt-in
  bool approve = false;   // ask before accepting new workers
  int port = 7420;
  char out_dir[1024] = {};
  char film_dir[1024] = {};
  std::string diagnostics;  // the last "Run diagnostics" output
  std::string note;         // the last action's result
};

// Join a render: this app as a worker of another controller (docs/GUIDE.md "Settings page").
struct JoinState {
  char address[256] = {};
  char code[32] = {};
  std::thread thread;
  std::atomic<bool> leave{false};
  std::atomic<bool> running{false};
  WorkerStatus status;
  std::string ended;  // why the last join ended
};

// "Ask before accepting new workers": the host's question, waiting for the UI's answer.
struct Approval {
  std::string question;
  std::promise<bool> answer;
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
  SettingsPage settings_page;
  JoinState join;
  bool cpu_too = false;  // this machine's CPU renders next to its GPUs
  int cpu_reserve = 2;   // CPU threads kept free for the UI
  std::mutex approvals_mutex;
  std::deque<std::shared_ptr<Approval>> approvals;
  std::string name(const HostRow& h) const {
    auto it = renamed.find(h.id);
    return it == renamed.end() ? h.name : it->second;
  }
};

void drawDashboard(Model& model, UiState& ui);
void drawHosts(Model& model, UiState& ui);
void drawHostDetail(Model& model, UiState& ui);
void drawRender(Model& model, UiState& ui);
void drawViewport(Model& model, UiState& ui);
// Loads the recent scenes list, and opens `scene` (from the command line) if given.
void initRenderPage(UiState& ui);
void drawSettings(Model& model, UiState& ui);
// Join a render: the worker screen that replaces the pages while this machine is another controller's worker.
void drawJoined(Model& model, UiState& ui);
bool joined(const UiState& ui);
void leaveJoin(UiState& ui);  // waits for the worker to leave
// Hosting with the Settings page's switches (LAN, internet, approval, port); restarts it when it runs.
void startHosting(HostingSession& session, UiState& ui);
// The "Accept this worker?" dialog, drawn over any page.
void drawApprovals(UiState& ui);
// This machine's devices as a --devices value (Hosts and Settings switches), "" = none.
std::string localDevices(const UiState& ui);

// Page header: title, subtitle and an optional right-aligned note.
void pageHeader(const char* title, const char* subtitle, const char* note = nullptr);

}  // namespace pen::ui
