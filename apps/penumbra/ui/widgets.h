#pragma once

#include <imgui.h>

#include <cstdint>
#include <deque>
#include <string>

namespace pen::ui {

// Building blocks of the mockups' screens (docs/ui-mockups), drawn with ImGui. Sizes are the mockups' CSS pixels;
// they scale with the monitor's content scale.

float px(float cssPixels);  // CSS pixels to screen pixels

// A panel (#1A1D21, 1 px border, 12 px corners) of `height` (0 = fit the content) with `padding` inside.
// Returns false when it is clipped; call endCard() either way.
bool beginCard(const char* id, float width, float height = 0, float padding = 20);
void endCard();

void cardLabel(const char* text);                 // 12 px uppercase, secondary: "HOSTS ONLINE"
void bigNumber(const char* fmt, ...) IM_FMTARGS(1);  // 36 px mono
void statNumber(const char* fmt, ...) IM_FMTARGS(1); // 24 px mono
void secondary(const char* fmt, ...) IM_FMTARGS(1);  // 14 px secondary
void small(const char* fmt, ...) IM_FMTARGS(1);      // 12 px secondary
void mono(float size, ImU32 color, const char* fmt, ...) IM_FMTARGS(3);

// A rounded bar, `fraction` 0-1 filled with `color`.
void progressBar(float fraction, ImU32 color, float height = 8, float width = -1);

// A line through the values (oldest left), scaled to [0, max]; max <= 0 uses the largest value.
void lineChart(const char* id, const std::deque<float>& values, size_t capacity, float max, ImU32 color,
               float height, bool fill = true);

// Buttons, 44 px high (mockup touch target).
bool primaryButton(const char* label, bool enabled = true);    // amber
bool secondaryButton(const char* label, bool enabled = true);  // outlined
bool dangerButton(const char* label, bool enabled = true);     // outlined, red text

// A rounded status label ("Rendering", "Idle", "Lost").
void pill(const char* text, ImU32 background, ImU32 foreground);

// A two-column row inside a card: label left, value right (mono).
void keyValue(const char* key, const char* fmt, ...) IM_FMTARGS(2);

std::string formatBytesShort(uint64_t bytes);  // "14.2 GB"
std::string formatRate(double samplesPerSecond);  // "186 M samples/s"
std::string formatDuration(double seconds);  // "36 m", "2 h 5 m"

}  // namespace pen::ui
