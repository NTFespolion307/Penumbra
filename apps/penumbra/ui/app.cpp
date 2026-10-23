#include "apps/penumbra/ui/ui.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "apps/penumbra/ui/gfx.h"
#include "apps/penumbra/ui/model.h"
#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/render_runner.h"
#include "apps/penumbra/ui/texture.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"
#include "controller/hosting/hosting_session.h"
#include "core/exit_codes.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "core/version.h"
#include "net/interfaces.h"
#include "net/join_code.h"

namespace pen::ui {

namespace {

struct NavEntry {
  Page page;
  const char* name;
};

constexpr NavEntry kNav[] = {
    {Page::Dashboard, "Dashboard"}, {Page::Hosts, "Hosts"},       {Page::Render, "Render"},
    {Page::Viewport, "Viewport"},   {Page::Settings, "Settings"},
};

constexpr float kSidebarWidth = 220;

struct State {
  UiState ui;
  std::string host_name;
  std::string renderer;
};

// The address to give workers: the first LAN address, else the first VPN one (refreshed with the model).
std::string workerAddress(uint16_t port) {
  std::string vpn;
  for (const net::HostAddress& a : net::listHostAddresses()) {
    if (a.kind == net::HostAddress::Kind::Lan) return fmt::format("{}:{}", a.address, port);
    if (vpn.empty() && a.kind == net::HostAddress::Kind::Vpn) vpn = fmt::format("{}:{}", a.address, port);
  }
  return vpn.empty() ? fmt::format("port {}", port) : vpn;
}

// LAN hosting starts with the app (docs/GUIDE.md "Render lifecycle", step 2); internet hosting (UPnP) is opt-in
// in Settings. The join code is the saved one, created on first use, never the config file.
void startLanHosting(HostingSession& session, UiState& ui) {
  try {
    bool created = false;
    const std::string code = net::loadOrCreateJoinCode(paths::userDataDir() / "join_code.txt", created);
    RenderSettings s = ui.settings;
    s.no_upnp = true;  // LAN only until internet hosting is switched on
    session.startHosting(s, code);
    ui.join_code = net::formatJoinCode(code);
    ui.hosting_error.clear();
  } catch (const std::exception& e) {
    ui.hosting_error = fmt::format("Not hosting: {}", e.what());
    spdlog::error("UI: cannot host: {}", e.what());
    session.addEvent(SessionEvent::Level::Error, ui.hosting_error);
  }
}

void glfwError(int code, const char* text) { spdlog::debug("GLFW error {}: {}", code, text); }

// A full-width sidebar entry, 44 px high, highlighted when current (mockup "nav").
bool navItem(const char* label, bool current) {
  const float w = ImGui::GetContentRegionAvail().x;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float h = 44 * ImGui::GetStyle().FontScaleDpi;
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (current || ImGui::IsItemHovered())
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), current ? color::kSelected : color::kPanel, 8);
  ImFont* font = current ? fonts().sans_medium : fonts().sans;
  const float size = size::kBody * ImGui::GetStyle().FontScaleDpi;
  dl->AddText(font, size, ImVec2(pos.x + 12 * ImGui::GetStyle().FontScaleDpi, pos.y + (h - size) / 2),
              current ? color::kText : color::kTextSecondary, label);
  return clicked;
}

void drawSidebar(State& st, const Model& model) {
  const float s = ImGui::GetStyle().FontScaleDpi;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, color::kSidebar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 24 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
  ImGui::BeginChild("sidebar", ImVec2(kSidebarWidth * s, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar(2);
  ImGui::Indent(12 * s);
  text(fonts().sans_semibold, size::kTitle, color::kText, "Penumbra");
  text(fonts().sans, size::kSmall, color::kTextSecondary, "Controller \xC2\xB7 %s", st.host_name.c_str());
  ImGui::Unindent(12 * s);
  ImGui::Dummy(ImVec2(0, 12 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4 * s));  // mockup: 4 px between entries
  for (const NavEntry& n : kNav) {
    const bool current = st.ui.page == n.page || (n.page == Page::Hosts && st.ui.page == Page::HostDetail);
    if (navItem(n.name, current)) st.ui.page = n.page;
  }
  ImGui::PopStyleVar();

  // Footer: hosting state (filled in with the session in step 2) and which renderer draws the UI.
  const float footer = 64 * s;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer - 24 * s);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), color::kBorder);
  ImGui::Dummy(ImVec2(0, 12 * s));
  ImGui::Indent(12 * s);
  {
    const bool hosting = !st.ui.join_code.empty();
    const ImVec2 dot = ImGui::GetCursorScreenPos();
    const float r = 4 * s, line = size::kSmall * s;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + r, dot.y + line / 2 + 1), r,
                                                hosting ? color::kOk : color::kDanger);
    ImGui::SetCursorScreenPos(ImVec2(dot.x + 16 * s, dot.y));
    text(fonts().sans, size::kSmall, color::kTextSecondary, "%s", hosting ? "Hosting on LAN" : "Not hosting");
    text(fonts().mono, size::kSmall, color::kTextSecondary, "%u host%s \xC2\xB7 %u GPU%s", model.hostsOnline(),
         model.hostsOnline() == 1 ? "" : "s", model.gpuCount(), model.gpuCount() == 1 ? "" : "s");
  }
  ImGui::Unindent(12 * s);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawPage(State& st, Model& model) {
  const float s = ImGui::GetStyle().FontScaleDpi;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, color::kBackground);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40 * s, 32 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar(2);
  switch (st.ui.page) {
    case Page::Dashboard: drawDashboard(model, st.ui); break;
    case Page::Hosts: drawHosts(model, st.ui); break;
    case Page::HostDetail: drawHostDetail(model, st.ui); break;
    case Page::Render: drawRender(model, st.ui); break;
    case Page::Viewport: pageHeader("Viewport", "The render in progress"); break;
    case Page::Settings:
      pageHeader("Settings", "Hosting, defaults and this machine. Join a render turns this machine into a worker.");
      break;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawFrame(State& st, Model& model) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("Penumbra", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
  drawSidebar(st, model);
  ImGui::SameLine();
  ImGui::PopStyleVar();
  drawPage(st, model);
  ImGui::End();
}

// The window and its renderer: Vulkan first, OpenGL 3.3 when Vulkan cannot present here (D-100).
// PENUMBRA_UI_RENDERER=opengl skips Vulkan (troubleshooting, virtual displays).
struct Window {
  GLFWwindow* glfw = nullptr;
  std::unique_ptr<Gfx> gfx;
};

Window openWindow(int width, int height) {
  Window w;
  std::string why;
  const char* forced = std::getenv("PENUMBRA_UI_RENDERER");
  const bool glOnly = forced && std::string_view(forced) == "opengl";
#if defined(PEN_UI_VULKAN)
  if (!glOnly) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    w.glfw = glfwCreateWindow(width, height, "Penumbra", nullptr, nullptr);
    if (w.glfw) {
      try {
        w.gfx = createVulkanGfx(w.glfw);
        return w;
      } catch (const std::exception& e) {
        why = e.what();
      }
      glfwDestroyWindow(w.glfw);
      w.glfw = nullptr;
    } else {
      why = "GLFW could not create a window";
    }
    spdlog::info("UI: Vulkan cannot draw this window ({}); using OpenGL 3.3", why);
  }
#endif
  if (glOnly) spdlog::info("UI: OpenGL 3.3 (PENUMBRA_UI_RENDERER=opengl)");
  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  w.glfw = glfwCreateWindow(width, height, "Penumbra", nullptr, nullptr);
  if (!w.glfw) throw std::runtime_error("neither Vulkan nor OpenGL 3.3 can open a window" +
                                       (why.empty() ? std::string() : " (Vulkan: " + why + ")"));
  try {
    w.gfx = createGlGfx(w.glfw);
  } catch (...) {
    glfwDestroyWindow(w.glfw);
    throw;
  }
  return w;
}

}  // namespace

int run(const RenderSettings& settings) {
  glfwSetErrorCallback(glfwError);
  if (!glfwInit()) {
    spdlog::error("the UI needs a display (no window system found); run 'penumbra --terminal ...' or "
                  "'penumbra --worker ...' on machines without one");
    return toInt(ExitCode::BadArguments);
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the UI keeps no layout file; its settings live in the config file
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  GLFWmonitor* monitor = glfwGetPrimaryMonitor();  // none on some virtual displays
  float scale = monitor ? ImGui_ImplGlfw_GetContentScaleForMonitor(monitor) : 1.0f;
  if (scale <= 0) scale = 1.0f;
  Window win;
  try {
    win = openWindow(static_cast<int>(1440 * scale), static_cast<int>(900 * scale));
  } catch (const std::exception& e) {
    spdlog::error("UI: {}", e.what());
    ImGui::DestroyContext();
    glfwTerminate();
    return toInt(ExitCode::BadArguments);
  }
  glfwSetWindowSizeLimits(win.glfw, static_cast<int>(1024 * scale), static_cast<int>(640 * scale), GLFW_DONT_CARE,
                          GLFW_DONT_CARE);
  applyTheme(scale);

  State st;
  st.ui.settings = settings;
  st.host_name = hostName();
  st.renderer = win.gfx->name();
  spdlog::info("UI: drawing with {}, content scale {:.2f}", st.renderer, scale);

  HostingSession session;
  startLanHosting(session, st.ui);
  Model model(session);
  RenderRunner runner(session);  // after the session: a running render stops before the session goes
  st.ui.runner = &runner;
  initRenderPage(st.ui);

  // Test only (screenshots of each page, D-047 style): PENUMBRA_TEST_UI_PAGE=hosts | host:<id> | render | ...
  if (const char* page = std::getenv("PENUMBRA_TEST_UI_PAGE"); page && *page) {
    const std::string_view p = page;
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_UI_PAGE): opening page {}", p);
    if (p == "hosts") st.ui.page = Page::Hosts;
    else if (p.rfind("host:", 0) == 0) {
      st.ui.page = Page::HostDetail;
      st.ui.host = static_cast<WorkerId>(std::strtoul(page + 5, nullptr, 10));
    } else if (p == "render") st.ui.page = Page::Render;
    else if (p == "viewport") st.ui.page = Page::Viewport;
    else if (p == "settings") st.ui.page = Page::Settings;
  }

  // Test only: PENUMBRA_TEST_UI_SCREENSHOT=<file.png> saves the frame shown 8 s after start (OpenGL; set
  // PENUMBRA_UI_RENDERER=opengl) and exits, so pages can be checked without clicking (D-047 style).
  std::string screenshot;
  if (const char* shot = std::getenv("PENUMBRA_TEST_UI_SCREENSHOT"); shot && *shot) {
    screenshot = shot;
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_UI_SCREENSHOT): saving {} after 8 s, then exiting", screenshot);
  }
  const auto started = std::chrono::steady_clock::now();
  bool captured = false;

  const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(color::kBackground);
  const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
  while (!glfwWindowShouldClose(win.glfw)) {
    // Host statistics refresh once per second (docs/GUIDE.md "UI"); 10 frames a second when idle keeps the UI
    // responsive at little CPU cost. Input wakes the loop at once.
    glfwWaitEventsTimeout(0.1);
    if (glfwGetWindowAttrib(win.glfw, GLFW_ICONIFIED)) continue;
    if (model.update()) {
      if (session.hosting()) st.ui.worker_address = workerAddress(session.port());
      session.setDisabled(st.ui.disabled_hosts, st.ui.disabled_devices);
    }
    ImageTexture::collect();
    win.gfx->newFrame();
    ImGui::NewFrame();
    drawFrame(st, model);
    ImGui::Render();
    if (!screenshot.empty() && std::chrono::steady_clock::now() - started > std::chrono::seconds(8)) {
      if (captured) break;  // the capture was written by the previous frame's render
      if (!win.gfx->captureNextFrame(screenshot)) {
        spdlog::error("PENUMBRA_TEST_UI_SCREENSHOT needs the OpenGL renderer (PENUMBRA_UI_RENDERER=opengl)");
        break;
      }
      captured = true;
    }
    try {
      win.gfx->render(clear);
    } catch (const std::exception& e) {
      spdlog::error("UI: drawing failed: {}", e.what());
      break;
    }
  }

  win.gfx.reset();
  ImGui::DestroyContext();
  glfwDestroyWindow(win.glfw);
  glfwTerminate();
  return 0;
}

}  // namespace pen::ui
