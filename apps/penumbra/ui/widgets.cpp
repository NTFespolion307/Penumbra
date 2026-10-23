#include "apps/penumbra/ui/widgets.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "apps/penumbra/ui/theme.h"

namespace pen::ui {

namespace {

std::string vformat(const char* fmt, va_list args) {
  char buf[512];
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  return buf;
}

void textIn(ImFont* font, float size, ImU32 color, const std::string& s) {
  ImGui::PushFont(font, size);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(s.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

bool button(const char* label, bool enabled, ImU32 bg, ImU32 bgHover, ImU32 fg, ImU32 border, ImFont* font) {
  const float h = px(44);
  ImGui::PushFont(font, size::kBody);
  const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
  ImGui::PopFont();
  const ImVec2 sz(textSize.x + px(36), h);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::InvisibleButton(label, sz);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::EndDisabled();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float alpha = enabled ? 1.0f : 0.4f;
  const auto fade = [&](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32((c >> 24) * alpha) << 24); };
  if (((bg >> 24) & 0xFF) != 0) dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), fade(hovered ? bgHover : bg), px(8));
  else if (hovered && enabled) dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), color::kSelected, px(8));
  if (((border >> 24) & 0xFF) != 0) dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), fade(border), px(8));
  const float fs = px(size::kBody);
  const char* idMark = std::strstr(label, "##");  // "Copy##join": the label is drawn without its ID part
  dl->AddText(font, fs, ImVec2(pos.x + px(18), pos.y + (h - fs) / 2), fade(fg), label, idMark);
  return clicked && enabled;
}

}  // namespace

float px(float css) { return css * ImGui::GetStyle().FontScaleDpi; }

bool beginCard(const char* id, float width, float height, float padding) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, color::kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, color::kBorder);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(12));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(padding), px(padding)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(10)));
  ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
  if (height <= 0) flags |= ImGuiChildFlags_AutoResizeY;
  return ImGui::BeginChild(id, ImVec2(width, height), flags, ImGuiWindowFlags_NoScrollbar);
}

void endCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(2);
}

void cardLabel(const char* text) {
  std::string up = text;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  textIn(fonts().sans, size::kSmall, color::kTextSecondary, up);
}

void bigNumber(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  textIn(fonts().mono_medium, size::kBigStat, color::kText, vformat(fmt, a));
  va_end(a);
}

void statNumber(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  textIn(fonts().mono_medium, size::kStat, color::kText, vformat(fmt, a));
  va_end(a);
}

void secondary(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  textIn(fonts().sans, size::kBody, color::kTextSecondary, vformat(fmt, a));
  va_end(a);
}

void small(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  textIn(fonts().sans, size::kSmall, color::kTextSecondary, vformat(fmt, a));
  va_end(a);
}

void mono(float size, ImU32 col, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  textIn(fonts().mono, size, col, vformat(fmt, a));
  va_end(a);
}

void progressBar(float fraction, ImU32 col, float height, float width) {
  const float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
  const float h = px(height);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), color::kBorder, h / 2);
  const float f = std::clamp(fraction, 0.0f, 1.0f);
  if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + std::max(h, w * f), p.y + h), col, h / 2);
  ImGui::Dummy(ImVec2(w, h));
}

void lineChart(const char* id, const std::deque<float>& values, size_t capacity, float max, ImU32 col, float height,
               bool fill) {
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = px(height);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, h));
  (void)id;
  if (values.size() < 2) return;
  float top = max;
  if (top <= 0)
    for (float v : values) top = std::max(top, v);
  if (top <= 0) top = 1;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const size_t n = values.size();
  const float step = w / float(std::max<size_t>(capacity, 2) - 1);
  const float x0 = p.x + w - step * float(n - 1);  // newest at the right edge
  std::vector<ImVec2> pts;
  pts.reserve(n);
  for (size_t i = 0; i < n; ++i)
    pts.emplace_back(x0 + step * float(i), p.y + h - h * std::clamp(values[i] / top, 0.0f, 1.0f));
  if (fill) {
    const ImU32 under = (col & 0x00FFFFFF) | (0x33u << 24);
    for (size_t i = 1; i < n; ++i)
      dl->AddQuadFilled(pts[i - 1], pts[i], ImVec2(pts[i].x, p.y + h), ImVec2(pts[i - 1].x, p.y + h), under);
  }
  dl->AddPolyline(pts.data(), int(n), col, ImDrawFlags_None, px(2));
}

bool primaryButton(const char* label, bool enabled) {
  return button(label, enabled, color::kAccent, color::kAccentHover, color::kAccentText, 0, fonts().sans_semibold);
}

bool secondaryButton(const char* label, bool enabled) {
  return button(label, enabled, 0, 0, color::kText, color::kButtonBorder, fonts().sans);
}

bool dangerButton(const char* label, bool enabled) {
  return button(label, enabled, 0, 0, color::kDanger, color::kButtonBorder, fonts().sans);
}

void pill(const char* text, ImU32 bg, ImU32 fg) {
  ImFont* f = fonts().sans_semibold;
  const float fs = px(size::kSmall);
  ImGui::PushFont(f, size::kSmall);
  const ImVec2 ts = ImGui::CalcTextSize(text);
  ImGui::PopFont();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz(ts.x + px(20), fs + px(8));
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, sz.y / 2);
  ImGui::GetWindowDrawList()->AddText(f, fs, ImVec2(p.x + px(10), p.y + px(4)), fg, text);
  ImGui::Dummy(sz);
}

void keyValue(const char* key, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  const std::string value = vformat(fmt, a);
  va_end(a);
  textIn(fonts().sans, size::kBody, color::kTextSecondary, key);
  ImGui::PushFont(fonts().mono, size::kBody);
  const float w = ImGui::CalcTextSize(value.c_str()).x;
  ImGui::PopFont();
  ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
  textIn(fonts().mono, size::kBody, color::kText, value);
}

std::string formatBytesShort(uint64_t b) {
  if (b >= 1000ull * 1000 * 1000 * 1000) return fmt::format("{:.1f} TB", b / 1e12);
  if (b >= 1000ull * 1000 * 1000) return fmt::format("{:.1f} GB", b / 1e9);
  if (b >= 1000ull * 1000) return fmt::format("{:.0f} MB", b / 1e6);
  return fmt::format("{:.0f} KB", b / 1e3);
}

std::string formatRate(double s) {
  if (s >= 1e9) return fmt::format("{:.2f} G samples/s", s / 1e9);
  if (s >= 1e6) return fmt::format("{:.0f} M samples/s", s / 1e6);
  if (s >= 1e3) return fmt::format("{:.0f} K samples/s", s / 1e3);
  return fmt::format("{:.0f} samples/s", s);
}

std::string formatDuration(double seconds) {
  const long s = long(seconds);
  if (s < 60) return fmt::format("{} s", s);
  if (s < 3600) return fmt::format("{} m", s / 60);
  return fmt::format("{} h {} m", s / 3600, (s % 3600) / 60);
}

}  // namespace pen::ui
