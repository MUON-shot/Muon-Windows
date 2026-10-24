#pragma once
#include "imgui.h"
#include "params.h"

namespace W {
extern ImU32 kAccent;
void SetupStyle(float dpi);
void SetFonts(ImFont* bold, ImFont* small_);

enum ButtonKind { BTN_DEFAULT = 0, BTN_PRIMARY, BTN_GHOST, BTN_DANGER };

// Lightroom-style slider: label + value on one line, thin track below.
// Double-click resets, Ctrl+click types a value, Shift drags finely.
bool Slider(const char* label, float* v, float mn, float mx, float def, const char* fmt = nullptr,
            const ImU32* grad = nullptr, int gradN = 0);
bool CurveEditor(const char* id, Curve& c, ImU32 color, const float* hist, float size);
bool ColorWheel(const char* id, Wheel& w, float size);
void Histogram(const float hist[4][256], ImVec2 size, bool* clipLo, bool* clipHi);
bool Segmented(const char* id, int* v, const char* const* items, int n, float height = 0);

// Buttons drawn by hand so every button in the app shares one look.
bool Button(const char* label, ImVec2 size = ImVec2(0, 0), int kind = BTN_DEFAULT, bool selected = false);
bool IconButton(const char* icon, const char* tooltip, int kind = BTN_GHOST, bool selected = false, float size = 0);
bool Toggle(const char* label, bool* v);
bool Combo(const char* label, int* v, const char* items);           // label left, combo right
bool Combo(const char* label, int* v, const char* const* items, int n);
// Collapsible section header. Shows a reset icon on hover; *reset is set when it is clicked.
bool Section(const char* title, bool defaultOpen = true, bool* reset = nullptr);
void Caption(const char* text);                                      // small dim helper text
void Hint(const char* text);
ImU32 HueColor(float hueDeg, float s = 0.75f, float v = 0.9f);

// kept for older call sites
inline bool SmallButton(const char* label, bool selected = false, float width = 0) { return Button(label, ImVec2(width, 0), BTN_DEFAULT, selected); }
}
