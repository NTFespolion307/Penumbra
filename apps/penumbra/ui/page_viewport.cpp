// Viewport (docs/GUIDE.md "Viewport page"; docs/ui-mockups/05-viewport): the render in progress.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fmt/format.h>

#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"
#include "core/output/output.h"
#include "core/system/paths.h"

namespace pen::ui {

namespace {

// Up to 8 distinct host colours (mockup: amber, blue, grey first); beyond 8 they repeat with a hatch.
constexpr ImU32 kHostColors[] = {color::rgb(0xF2A33A), color::rgb(0x3D7FD9), color::rgb(0x9AA3AD),
                                 color::rgb(0x4CC38A), color::rgb(0xB07CE8), color::rgb(0xE8655A),
                                 color::rgb(0x4FC1C9), color::rgb(0xE6D25A)};
constexpr int kHostColorCount = int(std::size(kHostColors));

ImU32 withAlpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(a * 255) << 24); }

// RGBA8 through the output's display transform (exposure, sRGB curve until M11).
void encode(const std::vector<Vec3f>& rgb, double ev, std::vector<uint8_t>& out) {
  const float scale = float(std::exp2(ev));
  out.resize(rgb.size() * 4);
  for (size_t i = 0; i < rgb.size(); ++i) {
    for (int c = 0; c < 3; ++c) out[i * 4 + c] = uint8_t(std::lround(255.0f * displayEncode(rgb[i][c], scale)));
    out[i * 4 + 3] = 255;
  }
}

struct ViewportState {
  LiveView view;  // the last one seen (stays after the render ends)
  uint64_t shown_version = ~0ull;
  double shown_ev = 1e9;
  ImageTexture image;
  std::vector<uint8_t> rgba;
  // Zoom: 1 = fit; pan in film pixels (the centre of the view).
  float zoom = 1;
  ImVec2 centre{-1, -1};
  bool overlay = true;
  // Full-resolution detail of the visible region when zoomed past the live view's resolution.
  ImageTexture detail;
  TileRect detail_rect;
  double detail_ev = 1e9;
  std::chrono::steady_clock::time_point detail_wanted{};
  std::chrono::steady_clock::time_point detail_at{};
  std::string save_note;
};

ViewportState& vs() {
  static ViewportState s;
  return s;
}

std::string lastCheckpoint(const Model& model) {
  for (auto it = model.events().rbegin(); it != model.events().rend(); ++it)
    if (it->text.rfind("Checkpoint", 0) == 0) {
      const std::time_t t = std::chrono::system_clock::to_time_t(it->time);
      std::tm tm{};
#if defined(_WIN32)
      localtime_s(&tm, &t);
#else
      localtime_r(&t, &tm);
#endif
      return fmt::format("Last checkpoint {:02}:{:02}", tm.tm_hour, tm.tm_min);
    }
  return "No checkpoint yet";
}

void drawImage(Model& model, UiState& ui, ViewportState& s, ImVec2 size) {
  const LiveView& v = s.view;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), color::rgb(0x0B0C0E), px(8));
  ImGui::InvisibleButton("##image", size);
  if (!s.image.valid() || !v.layout.width) {
    const char* msg = "The image appears here as soon as the first tiles are merged.";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(origin.x + (size.x - ts.x) / 2, origin.y + (size.y - ts.y) / 2), color::kTextSecondary, msg);
    return;
  }
  const float fw = float(v.layout.width), fh = float(v.layout.height);
  if (s.centre.x < 0) s.centre = ImVec2(fw / 2, fh / 2);
  const float fit = std::min(size.x / fw, size.y / fh);
  // Mouse: wheel zooms about the cursor, drag pans.
  if (ImGui::IsItemHovered()) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float before = fit * s.zoom;
      const ImVec2 filmAt(s.centre.x + (m.x - (origin.x + size.x / 2)) / before,
                          s.centre.y + (m.y - (origin.y + size.y / 2)) / before);
      s.zoom = std::clamp(s.zoom * std::pow(1.2f, wheel), 1.0f, std::max(1.0f, 64.0f / fit));
      const float after = fit * s.zoom;
      s.centre = ImVec2(filmAt.x - (m.x - (origin.x + size.x / 2)) / after, filmAt.y - (m.y - (origin.y + size.y / 2)) / after);
    }
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    s.centre.x -= d.x / (fit * s.zoom);
    s.centre.y -= d.y / (fit * s.zoom);
  }
  const float scale = fit * s.zoom;  // screen pixels per film pixel
  const float halfW = size.x / 2 / scale, halfH = size.y / 2 / scale;
  s.centre.x = std::clamp(s.centre.x, std::min(halfW, fw / 2), std::max(fw - halfW, fw / 2));
  s.centre.y = std::clamp(s.centre.y, std::min(halfH, fh / 2), std::max(fh - halfH, fh / 2));
  const ImVec2 imgMin(origin.x + size.x / 2 - s.centre.x * scale, origin.y + size.y / 2 - s.centre.y * scale);
  const ImVec2 imgMax(imgMin.x + fw * scale, imgMin.y + fh * scale);
  dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  dl->AddImage(s.image.ref(), imgMin, imgMax);

  // Zoomed past the live view's resolution: the visible region at full resolution, a moment after it settles.
  const bool beyond = scale > float(v.width) / fw * 1.5f;
  const auto now = std::chrono::steady_clock::now();
  if (beyond) {
    TileRect want{uint32_t(std::max(0.0f, s.centre.x - halfW)), uint32_t(std::max(0.0f, s.centre.y - halfH)),
                  uint32_t(std::min(fw, s.centre.x + halfW + 1)), uint32_t(std::min(fh, s.centre.y + halfH + 1))};
    const bool moved = !(want == s.detail_rect);
    if (moved) s.detail_wanted = now;
    const bool settled = now - s.detail_wanted > std::chrono::milliseconds(250);
    const bool stale = now - s.detail_at > std::chrono::seconds(2) || s.detail_ev != ui.runner->exposure();
    if (settled && (moved || stale) && want.pixels() > 0 && want.pixels() <= 4096u * 4096u) {
      std::vector<Vec3f> px;
      if (model.session().readFilm(want, px)) {
        std::vector<uint8_t> rgba;
        encode(px, ui.runner->exposure(), rgba);
        s.detail.set(rgba.data(), int(want.width()), int(want.height()));
        s.detail_rect = want;
        s.detail_ev = ui.runner->exposure();
        s.detail_at = now;
      }
    }
    if (s.detail.valid())
      dl->AddImage(s.detail.ref(), ImVec2(imgMin.x + s.detail_rect.x0 * scale, imgMin.y + s.detail_rect.y0 * scale),
                   ImVec2(imgMin.x + s.detail_rect.x1 * scale, imgMin.y + s.detail_rect.y1 * scale));
  }

  // Tiles coloured by the host that merged them, tiles in progress outlined. Small tiles are grouped so each
  // square is at least 6 px.
  if (s.overlay) {
    const float tileScreen = float(v.layout.tile_size) * scale;
    const uint32_t group = tileScreen >= px(6) ? 1u : uint32_t(std::ceil(px(6) / tileScreen));
    for (uint32_t ty = 0; ty < v.layout.tiles_y; ty += group)
      for (uint32_t tx = 0; tx < v.layout.tiles_x; tx += group) {
        int owner = -1;
        bool busy = false;
        for (uint32_t gy = ty; gy < std::min(ty + group, v.layout.tiles_y); ++gy)
          for (uint32_t gx = tx; gx < std::min(tx + group, v.layout.tiles_x); ++gx) {
            const uint32_t t = gy * v.layout.tiles_x + gx;
            if (owner < 0 && v.tile_owner[t] >= 0) owner = v.tile_owner[t];
            busy = busy || v.tile_busy[t];
          }
        const TileRect a = v.layout.tileRect(ty * v.layout.tiles_x + tx);
        const TileRect b = v.layout.tileRect(std::min(ty + group, v.layout.tiles_y) * v.layout.tiles_x - v.layout.tiles_x +
                                             std::min(tx + group, v.layout.tiles_x) - 1);
        const ImVec2 p0(imgMin.x + a.x0 * scale, imgMin.y + a.y0 * scale), p1(imgMin.x + b.x1 * scale, imgMin.y + b.y1 * scale);
        if (owner >= 0) {
          dl->AddRectFilled(p0, p1, withAlpha(kHostColors[owner % kHostColorCount], 0.15f));
          if (owner >= kHostColorCount)  // hatch for hosts past the 8th colour
            for (float x = p0.x; x < p1.x; x += px(6)) dl->AddLine(ImVec2(x, p0.y), ImVec2(std::min(p1.x, x + (p1.y - p0.y)), std::min(p1.y, p0.y + (p1.x - x))), withAlpha(color::kText, 0.25f));
        }
        if (busy) dl->AddRect(p0, p1, color::kText, 0, 0, px(2));
      }
  }
  dl->PopClipRect();
}

void progressPanel(Model& model, UiState& ui, ViewportState& s, float width) {
  const std::optional<RenderProgress> p = model.session().progress();
  beginCard("progress", width, 0);
  cardLabel("Progress");
  if (p) {
    const double done = p->samples_total ? double(p->samples_done) / double(p->samples_total) : 0;
    progressBar(float(done), color::kAccent, 10);
    statNumber("%.0f%%", 100 * done);
    keyValue("Tiles", "%u / %u", p->items_done, p->items_total);
    keyValue("Samples/s", "%s", formatRate(model.samplesPerSecond() > 0 ? model.samplesPerSecond()
                                           : (p->elapsed_seconds > 0 ? p->samples_done / p->elapsed_seconds : 0)).c_str());
    keyValue("Elapsed", "%s", formatDuration(p->elapsed_seconds).c_str());
    keyValue("ETA", "%s", done > 0.001 ? formatDuration(p->elapsed_seconds * (1 - done) / done).c_str() : "\xE2\x80\x94");
    small("%s", lastCheckpoint(model).c_str());
  } else {
    secondary("%s", ui.runner->state() == RenderRunner::State::Preparing ? "Preparing: packaging and loading the scene\xE2\x80\xA6"
                                                                         : "No render running.");
    const std::string msg = ui.runner->message();
    if (!msg.empty()) {
      ImGui::PushTextWrapPos(0);
      text(fonts().sans, size::kBody, ui.runner->state() == RenderRunner::State::Failed ? color::kDanger : color::kOk,
           "%s", msg.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  endCard();

  // Each host's share of the tiles merged so far.
  beginCard("shares", width, 0);
  cardLabel("Merged tiles per host");
  const LiveView& v = s.view;
  std::vector<uint32_t> count(v.workers.size(), 0);
  uint32_t total = 0;
  for (int32_t o : v.tile_owner)
    if (o >= 0 && size_t(o) < count.size()) {
      ++count[o];
      ++total;
    }
  if (!total) secondary("Nothing merged yet.");
  for (size_t i = 0; i < v.workers.size(); ++i) {
    if (!count[i]) continue;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x, p0.y + px(4)), ImVec2(p0.x + px(10), p0.y + px(14)),
                                              kHostColors[i % kHostColorCount], px(2));
    ImGui::Dummy(ImVec2(px(14), px(14)));
    ImGui::SameLine();
    keyValue(v.workers[i].c_str(), "%u \xC2\xB7 %.0f%%", count[i], 100.0 * count[i] / total);
    progressBar(float(count[i]) / float(total), kHostColors[i % kHostColorCount], 4);
  }
  endCard();

  beginCard("adjust", width, 0);
  cardLabel("Adjust while rendering");
  small("Exposure (EV)");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  float ev = float(ui.runner->exposure());
  if (ImGui::SliderFloat("##vev", &ev, -5.0f, 5.0f, "%+.1f")) ui.runner->setExposure(ev);
  small("Tonemap");
  ImGui::BeginDisabled();
  int srgb = 0;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::Combo("##vtm", &srgb, "sRGB curve\0");
  bool denoise = false;
  ImGui::Checkbox("Denoised preview", &denoise);
  ImGui::EndDisabled();
  ImGui::PushTextWrapPos(0);
  small("AgX/Filmic/ACES and the denoised preview arrive in M11. Exposure also applies to the output.");
  ImGui::PopTextWrapPos();
  endCard();
}

}  // namespace

void drawViewport(Model& model, UiState& ui) {
  ViewportState& s = vs();
  // The latest live view while a render runs; the last one stays after it ends.
  if (model.session().liveView(s.view, s.shown_version) &&
      (s.view.version != s.shown_version || s.shown_ev != ui.runner->exposure())) {
    encode(s.view.rgb, ui.runner->exposure(), s.rgba);
    s.image.set(s.rgba.data(), int(s.view.width), int(s.view.height));
    s.shown_version = s.view.version;
    s.shown_ev = ui.runner->exposure();
  } else if (s.image.valid() && s.shown_ev != ui.runner->exposure() && !s.view.rgb.empty()) {
    encode(s.view.rgb, ui.runner->exposure(), s.rgba);  // after the render: exposure still adjusts the last image
    s.image.set(s.rgba.data(), int(s.view.width), int(s.view.height));
    s.shown_ev = ui.runner->exposure();
  }

  const RenderSettings& rs = ui.runner->settings();
  const bool running = ui.runner->busy();
  const std::string subtitle =
      rs.scene.empty() ? std::string("No render yet")
                       : fmt::format("{} \xC2\xB7 {} \xC3\x97 {} \xC2\xB7 {} spp", pathToUtf8(rs.scene.filename()),
                                     rs.outputWidth(), rs.outputHeight(), rs.spp);
  text(fonts().sans_semibold, size::kHeading, color::kText, "Viewport");
  // Controls, right-aligned on the title row.
  ImGui::SameLine(ImGui::GetContentRegionMax().x - px(380));
  const bool paused = model.session().paused();
  if (secondaryButton(paused ? "Resume" : "Pause", running && ui.runner->state() == RenderRunner::State::Rendering))
    model.session().setPaused(!paused);
  ImGui::SameLine();
  if (secondaryButton("Save image now", running)) {
    RenderSettings out = rs;
    out.exposure = ui.runner->exposure();
    std::string error;
    s.save_note = model.session().writeImageNow(out, error) ? "Saved " + pathToUtf8(out.output) : "Not saved: " + error;
  }
  ImGui::SameLine();
  if (dangerButton("Stop", running)) ui.runner->stop();
  text(fonts().sans, size::kBody, color::kTextSecondary, "%s%s", subtitle.c_str(), paused ? "  \xC2\xB7  Paused" : "");
  if (!s.save_note.empty()) small("%s", s.save_note.c_str());
  ImGui::Dummy(ImVec2(0, px(8)));

  // View controls.
  if (ImGui::SmallButton("Fit")) {
    s.zoom = 1;
    s.centre = ImVec2(-1, -1);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("100%") && s.view.layout.width) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float fit = std::min((avail.x - px(316)) / float(s.view.layout.width), avail.y / float(s.view.layout.height));
    s.zoom = std::max(1.0f, 1.0f / fit);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(px(160));
  ImGui::SliderFloat("Zoom", &s.zoom, 1.0f, 64.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine(0, px(24));
  ImGui::Checkbox("Colour tiles by host", &s.overlay);

  const float gap = px(16), panel = px(300);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  drawImage(model, ui, s, ImVec2(avail.x - panel - gap, std::max(px(200), avail.y)));
  ImGui::SameLine(0, gap);
  ImGui::BeginChild("vpanel", ImVec2(panel, 0), 0, ImGuiWindowFlags_NoBackground);
  progressPanel(model, ui, s, panel);
  ImGui::EndChild();
}

}  // namespace pen::ui
