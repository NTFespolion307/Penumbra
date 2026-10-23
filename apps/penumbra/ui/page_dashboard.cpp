// Dashboard (docs/GUIDE.md "UI"; docs/ui-mockups/01-dashboard): what the cluster is doing right now.

#include <imgui.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>

#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"
#include "core/system/file.h"

namespace pen::ui {

void pageHeader(const char* title, const char* subtitle, const char* note) {
  text(fonts().sans_semibold, size::kHeading, color::kText, "%s", title);
  if (note) {
    ImGui::PushFont(fonts().mono, size::kSmall);
    const float w = ImGui::CalcTextSize(note).x;
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(14));
    mono(size::kSmall, color::kTextSecondary, "%s", note);
  }
  text(fonts().sans, size::kBody, color::kTextSecondary, "%s", subtitle);
  ImGui::Dummy(ImVec2(0, px(14)));
}

namespace {

std::string clock(std::chrono::system_clock::time_point t) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  return fmt::format("{:02}:{:02}", tm.tm_hour, tm.tm_min);
}

void renderCard(Model& model, UiState& ui, float width) {
  beginCard("render", width, 0, 24);
  cardLabel("Current render");
  if (const auto p = model.session().progress()) {
    const double done = p->samples_total ? double(p->samples_done) / double(p->samples_total) : 0;
    text(fonts().sans_semibold, size::kTitle, color::kText, "%s", ui.settings.scene.filename().string().c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - px(260));
    pill("Rendering", color::kAccentPill, color::kAccent);
    ImGui::SameLine();
    if (primaryButton("Open viewport")) ui.page = Page::Viewport;
    progressBar(float(done), color::kAccent, 10);
    statNumber("%.0f%%", 100 * done);
    ImGui::SameLine(0, px(32));
    statNumber("%u / %u", p->items_done, p->items_total);
    ImGui::SameLine(0, px(32));
    statNumber("%s", formatDuration(p->elapsed_seconds).c_str());
  } else {
    text(fonts().sans_semibold, size::kTitle, color::kText, "No render running");
    secondary("Pick a scene and start one on the Render page; connected workers join it.");
    if (primaryButton("Set up a render")) ui.page = Page::Render;
  }
  endCard();
}

}  // namespace

void drawDashboard(Model& model, UiState& ui) {
  pageHeader("Dashboard", "What the cluster is doing right now", "Updated every second");
  const float gap = px(16);
  const float full = ImGui::GetContentRegionAvail().x;
  renderCard(model, ui, full);
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

  // Metric cards: as many columns of at least 240 px as fit (mockup grid).
  const int cols = std::max(1, int((full + gap) / (px(240) + gap)));
  const float w = (full - gap * float(cols - 1)) / float(cols);
  const float h = px(206);
  int cell = 0;
  const auto next = [&] {
    if (++cell % cols != 0) ImGui::SameLine(0, gap);
    else ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
  };

  // Hosts online, with the join code and the address to give workers.
  beginCard("hosts", w, h);
  cardLabel("Hosts online");
  bigNumber("%u", model.hostsOnline());
  {
    std::string names;
    for (const HostRow& r : model.hosts())
      if (r.connected) names += (names.empty() ? "" : ", ") + ui.name(r);
    ImGui::PushTextWrapPos(0);
    secondary("%s", names.c_str());
    ImGui::PopTextWrapPos();
  }
  if (!ui.join_code.empty()) {
    mono(13, color::kText, "%s", ui.join_code.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ui.join_code.c_str());
    mono(size::kSmall, color::kTextSecondary, "%s", ui.worker_address.c_str());
  } else {
    small("%s", ui.hosting_error.empty() ? "Not hosting" : ui.hosting_error.c_str());
  }
  endCard();
  next();

  beginCard("gpus", w, h);
  cardLabel("GPUs across all hosts");
  bigNumber("%u", model.gpuCount());
  keyValue("Rendering", "%u", model.gpusRendering());
  keyValue("Idle", "%u", model.gpuCount() - model.gpusRendering());
  keyValue("Skipped", "%u", 0u);
  endCard();
  next();

  beginCard("cpu", w, h);
  cardLabel("Cluster CPU");
  bigNumber("%.0f%%", model.clusterCpu());
  progressBar(model.clusterCpu() / 100.0f, color::kInfo);
  secondary("%u threads on %u hosts, weighted by cores", model.clusterThreads(), model.hostsOnline());
  endCard();
  next();

  beginCard("ram", w, h);
  cardLabel("Cluster RAM");
  const uint64_t used = model.clusterRamUsed(), total = model.clusterRamTotal();
  bigNumber("%.0f / %.0f GB", used / 1e9, total / 1e9);
  progressBar(total ? float(double(used) / double(total)) : 0, color::kInfo);
  secondary("%.0f%% used across all hosts", total ? 100.0 * double(used) / double(total) : 0.0);
  endCard();
  next();

  beginCard("throughput", w, h);
  cardLabel("Throughput");
  small("last 10 minutes");
  statNumber("%s", formatRate(model.samplesPerSecond()).c_str());
  lineChart("tp", model.throughput().values(), History::kSamples, 0, color::kAccent, 56);
  endCard();
  next();

  beginCard("disk", w, h);
  cardLabel("Controller disk");
  std::filesystem::path out = std::filesystem::absolute(ui.settings.output).parent_path();
  bigNumber("%s free", formatBytesShort(freeDiskBytes(out)).c_str());
  ImGui::PushTextWrapPos(0);
  secondary("In %s. The Render page shows what a render needs.", out.string().c_str());
  ImGui::PopTextWrapPos();
  endCard();
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

  beginCard("events", full, 0);
  cardLabel("Events");
  const auto& events = model.events();
  if (events.empty()) secondary("Nothing yet.");
  const size_t first = events.size() > 20 ? events.size() - 20 : 0;
  for (size_t i = events.size(); i-- > first;) {
    const SessionEvent& e = events[i];
    mono(size::kSmall, color::kTextSecondary, "%s", clock(e.time).c_str());
    ImGui::SameLine(0, px(16));
    const ImU32 c = e.level == SessionEvent::Level::Error     ? color::kDanger
                    : e.level == SessionEvent::Level::Warning ? color::kAccent
                                                              : color::kText;
    text(fonts().sans, size::kBody, c, "%s", e.text.c_str());
  }
  endCard();
}

}  // namespace pen::ui
