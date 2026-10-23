// Hosts and Host detail (docs/GUIDE.md "UI"; docs/ui-mockups/02-hosts, 03-host-detail).

#include <imgui.h>

#include <algorithm>
#include <ctime>
#include <fmt/format.h>

#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"

namespace pen::ui {

namespace {

struct Status {
  const char* text;
  ImU32 bg, fg;
};

Status statusOf(const HostRow& h) {
  if (!h.connected) return {"Lost", color::rgb(0x3A1E1E), color::kDanger};
  if (h.upload_bound) return {"Upload-bound", color::rgb(0x1B2A3A), color::kInfo};
  if (h.in_render) return {"Rendering", color::kAccentPill, color::kAccent};
  return {"Idle", color::kSelected, color::kTextSecondary};
}

std::string gpuCount(const HostRow& h) {
  uint32_t gpus = 0;
  for (const GpuRow& g : h.gpus) gpus += g.backend != "CPU";
  return gpus ? std::to_string(gpus) : "0 (CPU)";
}

float avgGpuUse(const HostRow& h) {
  float sum = 0;
  int n = 0;
  for (const GpuRow& g : h.gpus)
    if (g.backend != "CPU" && g.usage >= 0) {
      sum += g.usage;
      ++n;
    }
  return n ? sum / float(n) : -1;
}

std::string since(std::chrono::system_clock::time_point t) {
  return formatDuration(std::chrono::duration<double>(std::chrono::system_clock::now() - t).count());
}

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

void joinCodeRow(const UiState& ui) {
  small("Join code");
  ImGui::SameLine();
  mono(13, color::kText, "%s", ui.join_code.empty() ? "not hosting" : ui.join_code.c_str());
  if (!ui.join_code.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ui.join_code.c_str());
  }
}

}  // namespace

void drawHosts(Model& model, UiState& ui) {
  pageHeader("Hosts", "Every machine in the session. Click a host for details.");
  joinCodeRow(ui);
  ImGui::Dummy(ImVec2(0, px(8)));

  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp;
  ImGui::PushStyleColor(ImGuiCol_TableRowBg, color::kPanel);
  ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, color::kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(12), px(12)));
  if (ImGui::BeginTable("hosts", 10, flags)) {
    const char* headers[] = {"Status", "Host", "OS", "Address", "CPU", "RAM", "GPUs", "Avg GPU use", "Tiles/s", "Ping"};
    const float weights[] = {1.1f, 1.8f, 1.3f, 1.4f, 0.6f, 1.1f, 0.7f, 0.9f, 0.7f, 0.6f};
    for (int i = 0; i < 10; ++i)
      ImGui::TableSetupColumn(headers[i], i == 0 ? ImGuiTableColumnFlags_NoSort : 0, weights[i]);
    ImGui::TableHeadersRow();

    std::vector<const HostRow*> rows;
    for (const HostRow& h : model.hosts()) rows.push_back(&h);
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
      const ImGuiTableColumnSortSpecs& s = sort->Specs[0];
      const auto key = [&](const HostRow* h) -> double {
        switch (s.ColumnIndex) {
          case 4: return h->cpu;
          case 5: return double(h->ram_used);
          case 6: return double(h->gpus.size());
          case 7: return avgGpuUse(*h);
          case 8: return h->tiles_per_s;
          case 9: return h->rtt_ms;
          default: return 0;
        }
      };
      std::stable_sort(rows.begin() + 1, rows.end(), [&](const HostRow* a, const HostRow* b) {  // this machine stays first
        if (s.ColumnIndex >= 1 && s.ColumnIndex <= 3) {
          const std::string& x = s.ColumnIndex == 1 ? a->name : s.ColumnIndex == 2 ? a->os : a->address;
          const std::string& y = s.ColumnIndex == 1 ? b->name : s.ColumnIndex == 2 ? b->os : b->address;
          return s.SortDirection == ImGuiSortDirection_Ascending ? x < y : y < x;
        }
        return s.SortDirection == ImGuiSortDirection_Ascending ? key(a) < key(b) : key(b) < key(a);
      });
    }

    for (const HostRow* h : rows) {
      ImGui::TableNextRow(0, px(56));
      ImGui::TableNextColumn();
      const Status st = statusOf(*h);
      pill(st.text, st.bg, st.fg);
      ImGui::SameLine();
      // The whole row opens the host's details.
      if (ImGui::Selectable(fmt::format("##row{}", h->id).c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0, px(40)))) {
        ui.host = h->id;
        ui.page = Page::HostDetail;
      }
      ImGui::TableNextColumn();
      text(fonts().sans_medium, size::kBody, color::kText, "%s", ui.name(*h).c_str());
      small("%s", h->role.c_str());
      ImGui::TableNextColumn();
      secondary("%s", h->os.c_str());
      ImGui::TableNextColumn();
      mono(13, color::kText, "%s", h->address.c_str());
      ImGui::TableNextColumn();
      mono(13, color::kText, "%s", h->cpu >= 0 ? fmt::format("{:.0f}%", h->cpu).c_str() : "\xE2\x80\x94");
      ImGui::TableNextColumn();
      mono(13, color::kText, "%.1f / %.0f GB", h->ram_used / 1e9, h->ram_total / 1e9);
      ImGui::TableNextColumn();
      mono(13, color::kText, "%s", gpuCount(*h).c_str());
      ImGui::TableNextColumn();
      const float use = avgGpuUse(*h);
      mono(13, color::kText, "%s", use >= 0 ? fmt::format("{:.0f}%", use).c_str() : "\xE2\x80\x94");
      ImGui::TableNextColumn();
      mono(13, color::kText, "%.2f", h->tiles_per_s);
      ImGui::TableNextColumn();
      mono(13, color::kText, "%s",
           h->local ? "\xE2\x80\x94" : h->rtt_ms > 0 ? fmt::format("{:.0f} ms", h->rtt_ms).c_str() : "\xE2\x80\x94");
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  ImGui::Dummy(ImVec2(0, px(8)));
  small("Status: Idle \xC2\xB7 Rendering \xC2\xB7 Upload-bound \xC2\xB7 Lost");
  small("GPU use from NVIDIA drivers; other GPUs show Penumbra's busy %%");
}

void drawHostDetail(Model& model, UiState& ui) {
  const HostRow* h = model.host(ui.host);
  ImGui::PushStyleColor(ImGuiCol_Text, color::kAccent);
  if (ImGui::Selectable("\xE2\x86\x90 All hosts", false, 0, ImGui::CalcTextSize("\xE2\x86\x90 All hosts")))
    ui.page = Page::Hosts;
  ImGui::PopStyleColor();
  if (!h) {
    pageHeader("Host", "This host left the session a while ago.");
    return;
  }

  // Header: name, status, facts, actions.
  text(fonts().sans_semibold, size::kHeading, color::kText, "%s", ui.name(*h).c_str());
  ImGui::SameLine(0, px(16));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(8));
  const Status st = statusOf(*h);
  pill(st.text, st.bg, st.fg);
  std::string facts = h->os + "  \xC2\xB7  Penumbra " + h->version + "  \xC2\xB7  " + h->address;
  if (!h->local) {
    facts += "  \xC2\xB7  " + std::string(h->connected ? "Connected " : "Left ") + since(h->since);
    if (h->rtt_ms > 0) facts += fmt::format("  \xC2\xB7  Ping {:.0f} ms", h->rtt_ms);
    facts += fmt::format("  \xC2\xB7  Upload {}/s", formatBytesShort(uint64_t(h->upload_bytes_per_s)));
  }
  secondary("%s", facts.c_str());
  ImGui::Dummy(ImVec2(0, px(4)));

  static char renameBuf[64] = {};
  if (secondaryButton("Rename")) {
    std::snprintf(renameBuf, sizeof(renameBuf), "%s", ui.name(*h).c_str());
    ImGui::OpenPopup("rename");
  }
  ImGui::SameLine();
  const bool hostOff = ui.disabled_hosts.count(h->id) > 0;
  if (secondaryButton(hostOff ? "Enable host" : "Disable host")) {
    if (hostOff) ui.disabled_hosts.erase(h->id);
    else ui.disabled_hosts.insert(h->id);
  }
  if (!h->local) {
    ImGui::SameLine();
    if (dangerButton("Kick", h->connected)) model.session().kick(h->id, "kicked from the controller's Hosts page");
  }
  if (ImGui::BeginPopup("rename")) {
    secondary("Name shown for this host");
    ImGui::SetNextItemWidth(px(260));
    const bool enter = ImGui::InputText("##name", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ImGui::Button("Save")) {
      if (renameBuf[0]) ui.renamed[h->id] = renameBuf;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (hostOff) small("Disabled: this host's devices get no work from the next render on.");
  ImGui::Dummy(ImVec2(0, px(12)));

  // CPU and RAM, with the last 10 minutes.
  const float gap = px(16);
  const float full = ImGui::GetContentRegionAvail().x;
  const float half = (full - gap) / 2;
  beginCard("cpu", half, px(190));
  cardLabel("CPU");
  ImGui::SameLine(ImGui::GetContentRegionMax().x - px(80));
  statNumber("%s", h->cpu >= 0 ? fmt::format("{:.0f}%", h->cpu).c_str() : "\xE2\x80\x94");
  small("%s%s%u threads", h->cpu_model.c_str(), h->cpu_model.empty() ? "" : " \xC2\xB7 ", h->cores);
  lineChart("cpuh", model.cpuHistory(h->id).values(), History::kSamples, 100, color::kInfo, 70);
  endCard();
  ImGui::SameLine(0, gap);
  beginCard("ram", half, px(190));
  cardLabel("RAM");
  ImGui::SameLine(ImGui::GetContentRegionMax().x - px(160));
  statNumber("%.1f / %.0f GB", h->ram_used / 1e9, h->ram_total / 1e9);
  small("used / total");
  lineChart("ramh", model.ramHistory(h->id).values(), History::kSamples, 1, color::kInfo, 70);
  endCard();
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

  // One card per device.
  text(fonts().sans_semibold, size::kTitle, color::kText, "Devices (%zu)", h->gpus.size());
  const int cols = std::max(1, int((full + gap) / (px(280) + gap)));
  const float w = (full - gap * float(cols - 1)) / float(cols);
  for (size_t i = 0; i < h->gpus.size(); ++i) {
    const GpuRow& g = h->gpus[i];
    if (i % cols != 0) ImGui::SameLine(0, gap);
    beginCard(fmt::format("gpu{}", i).c_str(), w, px(230));
    small("%s %zu", g.backend == "CPU" ? "Device" : "GPU", i);
    text(fonts().sans_semibold, size::kBody + 2, color::kText, "%s", g.name.c_str());
    const auto key = std::make_pair(h->id, uint32_t(i));
    bool enabled = !ui.disabled_devices.count(key);
    if (ImGui::Checkbox(fmt::format("Enabled##d{}", i).c_str(), &enabled)) {
      if (enabled) ui.disabled_devices.erase(key);
      else ui.disabled_devices.insert(key);
    }
    secondary("%s \xC2\xB7 %s", g.backend.c_str(), h->in_render ? "Tier 0 \xC2\xB7 all in VRAM" : "tier when a render starts");
    if (g.vram_total) {
      small("VRAM  %.1f / %.0f GB", g.vram_used / 1e9, g.vram_total / 1e9);
      progressBar(float(double(g.vram_used) / double(g.vram_total)), color::kInfo, 6);
    }
    mono(size::kBody, color::kText, "%s", g.usage >= 0 ? fmt::format("{:.0f}%", g.usage).c_str() : "\xE2\x80\x94");
    ImGui::SameLine(0, px(6));
    small(g.usage_from_driver ? "usage" : "busy");
    ImGui::SameLine(0, px(24));
    mono(size::kBody, color::kText, "%s",
         g.temperature_c > -273 ? fmt::format("{:.0f}\xC2\xB0" "C", g.temperature_c).c_str() : "\xE2\x80\x94");
    ImGui::SameLine(0, px(6));
    small("temp");
    ImGui::SameLine(0, px(24));
    mono(size::kBody, color::kText, "%.2f", g.tiles_per_s);
    ImGui::SameLine(0, px(6));
    small("tiles/s");
    endCard();
  }
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

  // Scene cache and log.
  beginCard("cache", half, px(150));
  cardLabel("Scene cache");
  if (h->local) {
    secondary("The controller sends scenes; it keeps no cache.");
  } else {
    statNumber("%s / %s", formatBytesShort(h->cache_used).c_str(), formatBytesShort(h->cache_limit).c_str());
    secondary("%u scene%s cached", h->cache_scenes, h->cache_scenes == 1 ? "" : "s");
  }
  endCard();
  ImGui::SameLine(0, gap);
  beginCard("log", half, px(150));
  cardLabel("Log from this host");
  if (h->log.empty()) secondary(h->local ? "This machine's log is in the log file (Settings)." : "No warnings or errors.");
  for (size_t i = h->log.size(); i-- > 0;) {
    const SessionEvent& e = h->log[i];
    mono(size::kSmall, color::kTextSecondary, "%s", clock(e.time).c_str());
    ImGui::SameLine(0, px(12));
    text(fonts().sans, size::kSmall, e.level == SessionEvent::Level::Error ? color::kDanger : color::kText, "%s",
         e.text.c_str());
  }
  endCard();
}

}  // namespace pen::ui
