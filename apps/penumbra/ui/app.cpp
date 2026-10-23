#include "apps/penumbra/ui/ui.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <spdlog/spdlog.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "apps/penumbra/ui/gfx.h"
#include "apps/penumbra/ui/theme.h"
#include "core/exit_codes.h"
#include "core/system/sysinfo.h"
#include "core/version.h"

namespace pen::ui {

namespace {

enum class Page { Dashboard, Hosts, Render, Viewport, Settings };

struct PageInfo {
  Page page;
  const char* name;
  const char* subtitle;
};

// Titles and subtitles as in docs/ui-mockups.
constexpr PageInfo kPages[] = {
    {Page::Dashboard, "Dashboard", "What the cluster is doing right now"},
    {Page::Hosts, "Hosts", "Every machine in the session. Click a host for details."},
    {Page::Render, "Render", "Pick a scene, set it up, start. Workers download the scene, then join in."},
    {Page::Viewport, "Viewport", "The render in progress"},
    {Page::Settings, "Settings", "Hosting, defaults and this machine. Join a render turns this machine into a worker."},
};

constexpr float kSidebarWidth = 220;

struct State {
  Page page = Page::Dashboard;
  std::string host_name;
  std::string renderer;
};

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

void drawSidebar(State& st) {
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
  for (const PageInfo& p : kPages)
    if (navItem(p.name, st.page == p.page)) st.page = p.page;
  ImGui::PopStyleVar();

  // Footer: hosting state (filled in with the session in step 2) and which renderer draws the UI.
  const float footer = 64 * s;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer - 24 * s);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), color::kBorder);
  ImGui::Dummy(ImVec2(0, 12 * s));
  ImGui::Indent(12 * s);
  text(fonts().sans, size::kSmall, color::kTextSecondary, "Not hosting yet");
  text(fonts().mono, size::kSmall, color::kTextSecondary, "UI: %s", st.renderer.c_str());
  ImGui::Unindent(12 * s);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawPage(const State& st) {
  const float s = ImGui::GetStyle().FontScaleDpi;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, color::kBackground);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40 * s, 32 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar(2);
  for (const PageInfo& p : kPages) {
    if (p.page != st.page) continue;
    text(fonts().sans_semibold, size::kHeading, color::kText, "%s", p.name);
    text(fonts().sans, size::kBody, color::kTextSecondary, "%s", p.subtitle);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawFrame(State& st) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("Penumbra", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
  drawSidebar(st);
  ImGui::SameLine();
  ImGui::PopStyleVar();
  drawPage(st);
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
  (void)settings;  // the Render page uses them (step 4)
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
  st.host_name = hostName();
  st.renderer = win.gfx->name();
  spdlog::info("UI: drawing with {}, content scale {:.2f}", st.renderer, scale);

  const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(color::kBackground);
  const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
  while (!glfwWindowShouldClose(win.glfw)) {
    // Host statistics refresh once per second (docs/GUIDE.md "UI"); 10 frames a second when idle keeps the UI
    // responsive at little CPU cost. Input wakes the loop at once.
    glfwWaitEventsTimeout(0.1);
    if (glfwGetWindowAttrib(win.glfw, GLFW_ICONIFIED)) continue;
    win.gfx->newFrame();
    ImGui::NewFrame();
    drawFrame(st);
    ImGui::Render();
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
