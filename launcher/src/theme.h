// theme.h - the Far Cry 2 look, drawn entirely in code (no image files): sun-bleached sand and rust colours,
// a dusk backdrop with an acacia silhouette and map contour lines, torn painted-tape section strips.
#pragma once
#include "imgui.h"

namespace theme {
// palette
inline ImU32 Sand()    { return IM_COL32(232, 220, 196, 255); }
inline ImU32 Faded()   { return IM_COL32(176, 162, 136, 255); }
inline ImU32 Rust()    { return IM_COL32(181, 84, 28, 255); }
inline ImU32 Ochre()   { return IM_COL32(210, 154, 58, 255); }
inline ImU32 Ink()     { return IM_COL32(36, 26, 18, 255); }
inline ImU32 Good()    { return IM_COL32(150, 180, 90, 255); }
inline ImU32 Warn()    { return IM_COL32(226, 170, 60, 255); }
inline ImU32 Bad()     { return IM_COL32(214, 90, 60, 255); }

struct Fonts { ImFont* body = nullptr; ImFont* small = nullptr; ImFont* head = nullptr; ImFont* side = nullptr; ImFont* page = nullptr; ImFont* title = nullptr; };
extern Fonts g_fonts;
void LoadFonts(float scale);       // rebuilds the atlas (call before NewFrame; invalidate device objects first on rebuild)
void ApplyStyle(float scale);

void Backdrop();                                        // whole-window background (call first, on the root window)
void PaintStrip(ImVec2 a, ImVec2 b, ImU32 col, unsigned seed);   // torn painted tape between a and b
void TitleBlock(const char* title, const char* subtitle, const char* version);
bool SidebarButton(const char* label, bool selected);
void PageHeader(const char* title, const char* subtitle);
void Section(const char* name);
bool Button(const char* label, bool primary = false, bool enabled = true, float width = 0);
bool Checkbox(const char* label, bool* v, const char* help = nullptr);
void Wrapped(const char* text, ImU32 col = 0);
void Mark(char kind);                                   // '+', '!', 'x' report marks
}
