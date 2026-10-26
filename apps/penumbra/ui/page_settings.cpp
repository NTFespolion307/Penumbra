// Settings and Join a render (docs/GUIDE.md "Settings page"; docs/ui-mockups/06-settings).

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>

#include "apps/penumbra/ui/file_dialog.h"
#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"
#include "core/system/file.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "net/interfaces.h"
#include "net/join_code.h"
#include "worker/device_manager.h"

namespace pen::ui {

namespace {

std::filesystem::path codeFile() { return paths::userDataDir() / "join_code.txt"; }

// The UI's answer to "Accept this worker?": queued for the dialog; no answer within 2 minutes is a no.
bool askUi(UiState& ui, const std::string& question) {
  auto a = std::make_shared<Approval>();
  a->question = question;
  std::future<bool> answer = a->answer.get_future();
  {
    std::lock_guard lock(ui.approvals_mutex);
    ui.approvals.push_back(a);
  }
  if (answer.wait_for(std::chrono::minutes(2)) != std::future_status::ready) return false;
  return answer.get();
}

// The defaults the Settings page edits live in the user config file (docs/GUIDE.md "Configuration"), with the
// other keys kept as they are. Never the join code.
void saveDefaults(const UiState& ui, std::string& note) {
  const std::filesystem::path file = paths::userConfigFile();
  nlohmann::json j = nlohmann::json::object();
  try {
    std::ifstream in(file);
    if (in) j = nlohmann::json::parse(in);
    if (!j.is_object()) j = nlohmann::json::object();
    if (ui.settings_page.out_dir[0])
      j["output"] = pathToUtf8(pathFromUtf8(ui.settings_page.out_dir) / ui.settings.output.filename());
    if (ui.settings_page.film_dir[0]) j["film_dir"] = ui.settings_page.film_dir;
    else j.erase("film_dir");
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    writeFileAtomically(file, j.dump(2) + "\n");
    note = "Saved to " + pathToUtf8(file);
  } catch (const std::exception& e) {
    note = std::string("Could not save the defaults: ") + e.what();
  }
}

void toggleRow(const char* label, const char* hint, bool* value, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  ImGui::Checkbox(label, value);
  ImGui::EndDisabled();
  if (hint) {
    ImGui::SameLine(0, px(12));
    small("%s", hint);
  }
}

void hostingCard(Model& model, UiState& ui, float w) {
  HostingSession& session = model.session();
  SettingsPage& sp = ui.settings_page;
  const bool rendering = ui.runner->busy();
  beginCard("hosting", w, 0);
  cardLabel("Hosting");
  bool lan = sp.lan;
  toggleRow("LAN hosting", rendering ? "(not while rendering)" : nullptr, &lan, !rendering);
  if (lan != sp.lan) {
    sp.lan = lan;
    if (lan) startHosting(session, ui);
    else {
      session.stopHosting(std::chrono::seconds(0));
      ui.join_code.clear();
    }
  }
  bool internet = sp.internet;
  toggleRow("Internet hosting", "Tries UPnP; otherwise forward the TCP port on the router", &internet, sp.lan);
  if (internet != sp.internet) {
    sp.internet = internet;
    session.setInternet(internet);
  }
  bool approve = sp.approve;
  toggleRow("Ask before accepting new workers", nullptr, &approve, sp.lan);
  if (approve != sp.approve) {
    sp.approve = approve;
    session.setApproval(approve, [&ui](const std::string& q) { return askUi(ui, q); });
  }
  ImGui::Dummy(ImVec2(0, px(6)));
  small("Join code");
  mono(15, color::kText, "%s", ui.join_code.empty() ? "not hosting" : ui.join_code.c_str());
  ImGui::SameLine(0, px(12));
  if (secondaryButton("Copy", !ui.join_code.empty())) ImGui::SetClipboardText(ui.join_code.c_str());
  ImGui::SameLine();
  if (secondaryButton("Regenerate", sp.lan && !rendering)) {
    // A new code; every worker using the old one is disconnected (docs/GUIDE.md "Join code").
    std::error_code ec;
    std::filesystem::remove(codeFile(), ec);
    startHosting(session, ui);
    sp.note = "New join code; workers must join again with it";
  }
  small("Kept across restarts. Regenerate disconnects every worker using it.");
  ImGui::Dummy(ImVec2(0, px(6)));
  small("Addresses to give workers");
  if (session.hosting()) {
    for (const net::HostAddress& a : net::listHostAddresses())
      mono(13, color::kText, a.address.find(':') == std::string::npos ? "%s  %s:%u  (%s)" : "%s  [%s]:%u  (%s)",
           net::toString(a.kind), a.address.c_str(), unsigned(session.port()), a.interface.c_str());
    mono(13, color::kText, "Internet: %s", session.internetStatus().c_str());
  } else {
    secondary("Not hosting.");
  }
  ImGui::Dummy(ImVec2(0, px(6)));
  small("Port");
  ImGui::SetNextItemWidth(px(120));
  ImGui::InputInt("##port", &sp.port, 0);
  sp.port = std::clamp(sp.port, 1, 65535);
  ImGui::SameLine();
  if (secondaryButton("Apply", sp.lan && !rendering && sp.port != int(ui.settings.port))) {
    ui.settings.port = uint16_t(sp.port);
    startHosting(session, ui);
  }
  endCard();
}

void joinCard(Model& model, UiState& ui, float w) {
  JoinState& j = ui.join;
  beginCard("join", w, 0);
  cardLabel("Join a render");
  secondary("Stops hosting on this machine and lends its devices to another controller.");
  small("Controller address");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputTextWithHint("##jaddr", "192.168.1.20 or 100.101.102.103:7420", j.address, sizeof(j.address));
  small("Join code");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputTextWithHint("##jcode", "XXXX-XXXX-XXXX-XXXX", j.code, sizeof(j.code),
                           ImGuiInputTextFlags_CharsUppercase);
  // Test only (D-047 style): PENUMBRA_TEST_UI_JOIN="<address> <code>" fills the fields and presses Join once.
  static const std::string autoJoin = [] {
    const char* v = std::getenv("PENUMBRA_TEST_UI_JOIN");
    if (v && *v) spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_UI_JOIN): joining {}", v);
    return std::string(v ? v : "");
  }();
  static bool autoJoined = false;
  bool autoGo = false;
  if (!autoJoin.empty() && !autoJoined && autoJoin.find(' ') != std::string::npos) {
    autoJoined = autoGo = true;
    std::snprintf(j.address, sizeof(j.address), "%s", autoJoin.substr(0, autoJoin.find(' ')).c_str());
    std::snprintf(j.code, sizeof(j.code), "%s", autoJoin.substr(autoJoin.find(' ') + 1).c_str());
  }
  const bool valid = j.address[0] && net::normalizeJoinCode(j.code).has_value();
  const bool rendering = ui.runner->busy();
  if ((primaryButton("Join", valid && !rendering && !joined(ui)) || autoGo) && valid && !rendering && !joined(ui)) {
    model.session().stopHosting(std::chrono::seconds(0));
    ui.join_code.clear();
    WorkerOptions opt;
    opt.settings = ui.settings;
    opt.settings.devices = localDevices(ui).empty() ? "cpu" : localDevices(ui);
    if (opt.settings.threads == 0)
      opt.settings.threads = uint32_t(std::max(1, int(usableCpuThreads()) - ui.cpu_reserve));
    opt.settings.stay = true;  // reconnects like an always-on worker until Leave
    opt.join = j.address;
    opt.code = j.code;
    opt.interrupt = &j.leave;
    opt.status = &j.status;
    j.leave = false;
    j.running = true;
    {
      std::lock_guard lock(j.status.mutex);
      j.status.phase = "Starting";
      j.status.error.clear();
    }
    if (j.thread.joinable()) j.thread.join();
    j.thread = std::thread([&j, opt] {
      const int rc = runWorker(opt);
      {
        std::lock_guard lock(j.status.mutex);
        j.ended = j.status.error.empty() ? fmt::format("Left (exit code {})", rc) : j.status.error;
      }
      j.running = false;
    });
    ui.page = Page::Dashboard;
  }
  if (rendering) small("Not while this machine is rendering.");
  if (!j.ended.empty()) small("Last time: %s", j.ended.c_str());
  endCard();
}

void devicesCard(UiState& ui, float w) {
  beginCard("devices", w, 0);
  cardLabel("This machine's devices");
  const auto& gpus = gpuInventory().gpus;
  for (uint32_t i = 0; i < gpus.size(); ++i) {
    const auto key = std::make_pair(WorkerId(0), i);
    bool on = !ui.disabled_devices.count(key);
    if (ImGui::Checkbox(fmt::format("GPU {}  {}##g{}", i, gpus[i].name, i).c_str(), &on)) {
      if (on) ui.disabled_devices.erase(key);
      else ui.disabled_devices.insert(key);
    }
  }
  const SystemInfo sys = querySystem();
  if (gpus.empty()) {
    secondary("CPU renders locally \xC2\xB7 %s \xC2\xB7 no GPU that can render", sys.cpu.c_str());
  } else {
    toggleRow("CPU renders too", sys.cpu.c_str(), &ui.cpu_too);
  }
  small("CPU threads kept free for the UI");
  ImGui::SetNextItemWidth(px(200));
  ImGui::SliderInt("##reserve", &ui.cpu_reserve, 0, std::max(0, int(usableCpuThreads()) - 1));
  endCard();
}

void defaultsCard(UiState& ui, float w) {
  SettingsPage& sp = ui.settings_page;
  beginCard("defaults", w, 0);
  cardLabel("Defaults and storage");
  small("Default output folder");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputText("##outdir", sp.out_dir, sizeof(sp.out_dir));
  small("Film folder (large renders; empty = the output folder)");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputText("##filmdir", sp.film_dir, sizeof(sp.film_dir));
  small("Default tonemap");
  ImGui::BeginDisabled();
  int srgb = 0;
  ImGui::SetNextItemWidth(px(200));
  ImGui::Combo("##dtm", &srgb, "sRGB curve (AgX in M11)\0");
  ImGui::EndDisabled();
  if (primaryButton("Save defaults")) saveDefaults(ui, sp.note);
  ImGui::SameLine();
  if (secondaryButton("Open logs folder")) openFolder(paths::logDir());
  ImGui::SameLine();
  if (secondaryButton("Run diagnostics")) {
    sp.diagnostics = runSelf("--diagnose");
    ImGui::OpenPopup("Diagnostics");
  }
  if (!sp.note.empty()) small("%s", sp.note.c_str());
  ImGui::SetNextWindowSize(ImVec2(px(760), px(520)), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("Diagnostics", nullptr)) {
    ImGui::PushFont(fonts().mono, 13);
    ImGui::InputTextMultiline("##diag", sp.diagnostics.data(), sp.diagnostics.size() + 1,
                              ImVec2(-1, -px(56)), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
    if (secondaryButton("Copy")) ImGui::SetClipboardText(sp.diagnostics.c_str());
    ImGui::SameLine();
    if (primaryButton("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  endCard();
}

}  // namespace

std::string localDevices(const UiState& ui) {
  if (ui.disabled_hosts.count(0)) return "";
  const auto& gpus = gpuInventory().gpus;
  if (gpus.empty()) return "cpu";
  std::string list;
  for (uint32_t i = 0; i < gpus.size(); ++i)
    if (!ui.disabled_devices.count({0u, i})) list += (list.empty() ? "" : ",") + std::to_string(i);
  if (ui.cpu_too) list += list.empty() ? "cpu" : ",cpu";
  return list;
}

void startHosting(HostingSession& session, UiState& ui) {
  if (session.hosting()) session.stopHosting(std::chrono::seconds(0));
  const SettingsPage& sp = ui.settings_page;
  try {
    bool created = false;
    const std::string code = net::loadOrCreateJoinCode(codeFile(), created);
    RenderSettings s = ui.settings;
    s.port = uint16_t(sp.port);
    s.no_upnp = true;  // internet hosting (UPnP) is switched on separately
    s.approve_workers = sp.approve;
    session.startHosting(s, code);
    session.setApproval(sp.approve, [&ui](const std::string& q) { return askUi(ui, q); });
    if (sp.internet) session.setInternet(true);
    ui.join_code = net::formatJoinCode(code);
    ui.hosting_error.clear();
  } catch (const std::exception& e) {
    ui.join_code.clear();
    ui.hosting_error = fmt::format("Not hosting: {}", e.what());
    spdlog::error("UI: cannot host: {}", e.what());
    session.addEvent(SessionEvent::Level::Error, ui.hosting_error);
  }
}

void drawSettings(Model& model, UiState& ui) {
  pageHeader("Settings", "Hosting, defaults and this machine. Join a render turns this machine into a worker.");
  const float gap = px(16), full = ImGui::GetContentRegionAvail().x, half = (full - gap) / 2;
  ImGui::BeginGroup();
  hostingCard(model, ui, half);
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
  devicesCard(ui, half);
  ImGui::EndGroup();
  ImGui::SameLine(0, gap);
  ImGui::BeginGroup();
  joinCard(model, ui, half);
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
  defaultsCard(ui, half);
  ImGui::EndGroup();
}

bool joined(const UiState& ui) { return ui.join.running.load(); }

void leaveJoin(UiState& ui) {
  ui.join.leave = true;
  if (ui.join.thread.joinable()) ui.join.thread.join();
}

void drawJoined(Model& model, UiState& ui) {
  (void)model;
  JoinState& j = ui.join;
  WorkerStatus& st = j.status;
  std::lock_guard lock(st.mutex);
  pageHeader("Joined a render", "This machine is a worker of another controller. Leave to host again.");
  beginCard("joined", ImGui::GetContentRegionAvail().x, 0, 24);
  cardLabel("Controller");
  statNumber("%s", st.controller.c_str());
  secondary("%s%s", st.phase.c_str(), st.scene.empty() ? "" : (" \xC2\xB7 " + st.scene).c_str());
  if (st.phase == "Downloading the scene") progressBar(float(st.download), color::kAccent, 8);
  if (!st.error.empty()) text(fonts().sans, size::kBody, color::kDanger, "%s", st.error.c_str());
  keyValue("Upload", "%s/s", formatBytesShort(uint64_t(st.upload_bytes_per_s)).c_str());
  endCard();
  ImGui::Dummy(ImVec2(0, px(8)));
  beginCard("jdevices", ImGui::GetContentRegionAvail().x, 0);
  cardLabel("This machine's devices");
  for (const WorkerStatus::Device& d : st.devices)
    keyValue(fmt::format("{} ({})", d.name, d.backend).c_str(), "%.0f%% \xC2\xB7 %llu tiles", d.busy_percent,
             static_cast<unsigned long long>(d.tiles));
  endCard();
  ImGui::Dummy(ImVec2(0, px(8)));
  if (dangerButton("Leave")) {
    j.leave = true;  // hands unfinished leases back; the worker thread then ends and hosting starts again
  }
}

void drawApprovals(UiState& ui) {
  std::shared_ptr<Approval> a;
  {
    std::lock_guard lock(ui.approvals_mutex);
    if (!ui.approvals.empty()) a = ui.approvals.front();
  }
  if (!a) return;
  if (!ImGui::IsPopupOpen("Accept worker?")) ImGui::OpenPopup("Accept worker?");
  if (ImGui::BeginPopupModal("Accept worker?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    secondary("%s", a->question.c_str());
    int answer = -1;
    if (primaryButton("Accept")) answer = 1;
    ImGui::SameLine();
    if (secondaryButton("Reject")) answer = 0;
    if (answer >= 0) {
      a->answer.set_value(answer == 1);
      std::lock_guard lock(ui.approvals_mutex);
      ui.approvals.pop_front();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace pen::ui
