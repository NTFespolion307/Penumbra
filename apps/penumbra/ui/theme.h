#pragma once

#include <imgui.h>

namespace pen::ui {

// The approved mockups' visual language (docs/ui-mockups/README.md): graphite panels, amber accent, IBM Plex.
namespace color {
inline constexpr ImU32 rgb(unsigned v) {
  return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 0xFF);
}
inline const ImU32 kBackground = rgb(0x121417);
inline const ImU32 kSidebar = rgb(0x16191D);
inline const ImU32 kPanel = rgb(0x1A1D21);
inline const ImU32 kBorder = rgb(0x2A2E34);
inline const ImU32 kSelected = rgb(0x262A30);  // current page in the sidebar
inline const ImU32 kButtonBorder = rgb(0x3A3F46);
inline const ImU32 kText = rgb(0xE8EAED);
inline const ImU32 kTextSecondary = rgb(0xA3AAB3);
inline const ImU32 kAccent = rgb(0xF2A33A);       // amber
inline const ImU32 kAccentHover = rgb(0xFFC371);
inline const ImU32 kAccentText = rgb(0x1A1206);   // text on amber
inline const ImU32 kAccentPill = rgb(0x3A2C14);   // "Rendering" pill background
inline const ImU32 kInfo = rgb(0x5AA9F6);         // blue bars
inline const ImU32 kOk = rgb(0x4CC38A);
inline const ImU32 kDanger = rgb(0xFF8A80);
}  // namespace color

// Font faces; any size is drawn from one face (ImGui 1.92 dynamic fonts).
struct Fonts {
  ImFont* sans = nullptr;           // 400
  ImFont* sans_medium = nullptr;    // 500
  ImFont* sans_semibold = nullptr;  // 600
  ImFont* mono = nullptr;           // 400
  ImFont* mono_medium = nullptr;    // 500
};

// Loads the embedded fonts and sets ImGui's style to the mockups'. `scale` is the monitor's content scale.
Fonts applyTheme(float scale);
const Fonts& fonts();

// Sizes in the mockups' CSS pixels (scaled by the content scale through ImGui's font scale).
namespace size {
inline constexpr float kBody = 14, kSmall = 12, kTitle = 20, kHeading = 28, kStat = 24, kBigStat = 36;
}

// Text in a face, size and colour (ImGui::Text with PushFont/PushStyleColor around it).
void text(ImFont* font, float size, ImU32 col, const char* fmt, ...) IM_FMTARGS(4);

}  // namespace pen::ui
