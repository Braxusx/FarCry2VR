// theme.cpp - see theme.h
#include "theme.h"
#include "imgui_internal.h"
#include <windows.h>
#include <cmath>
#include <cstdio>

namespace theme {
Fonts g_fonts;

static float Rand(unsigned& s) { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); }

static const char* FirstFont(const char* const* names, int n) {
    static char path[MAX_PATH];
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    for (int i = 0; i < n; ++i) { snprintf(path, MAX_PATH, "%s\\Fonts\\%s", win, names[i]); if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path; }
    return nullptr;
}
void LoadFonts(float s) {
    ImGuiIO& io = ImGui::GetIO(); io.Fonts->Clear();
    static const char* body[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
    static const char* bold[] = { "seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf" };
    static const char* stencil[] = { "STENCIL.TTF", "stencil.ttf", "bahnschrift.ttf", "impact.ttf", "arialbd.ttf" };
    static const char* cond[] = { "bahnschrift.ttf", "seguisb.ttf", "arialbd.ttf" };
    const char* fb = FirstFont(body, 3); const char* fbb = FirstFont(bold, 4); const char* fs = FirstFont(stencil, 5); const char* fc = FirstFont(cond, 3);
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x2713, 0x2717, 0 };
    auto load = [&](const char* f, float px) { ImFont* r = f ? io.Fonts->AddFontFromFileTTF(f, px * s, &cfg, ranges) : nullptr; return r ? r : io.Fonts->AddFontDefault(); };
    g_fonts.body = load(fb, 16.0f);
    g_fonts.small = load(fb, 13.5f);
    g_fonts.head = load(fc, 17.0f);
    g_fonts.side = load(fc, 19.0f);
    g_fonts.page = load(fs, 30.0f);
    g_fonts.title = load(fs, 44.0f);
    (void)fbb;
    io.FontDefault = g_fonts.body;
}
void ApplyStyle(float s) {
    ImGuiStyle st; ImGui::StyleColorsDark(&st);
    ImVec4* c = st.Colors;
    auto C = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); };
    c[ImGuiCol_Text] = C(232, 220, 196); c[ImGuiCol_TextDisabled] = C(140, 128, 106);
    c[ImGuiCol_WindowBg] = C(0, 0, 0, 0); c[ImGuiCol_ChildBg] = C(0, 0, 0, 0); c[ImGuiCol_PopupBg] = C(34, 26, 20, 248);
    c[ImGuiCol_Border] = C(110, 86, 60, 200); c[ImGuiCol_FrameBg] = C(30, 23, 17, 230); c[ImGuiCol_FrameBgHovered] = C(52, 38, 26, 240); c[ImGuiCol_FrameBgActive] = C(70, 48, 30, 255);
    c[ImGuiCol_CheckMark] = C(226, 150, 60); c[ImGuiCol_SliderGrab] = C(210, 140, 60); c[ImGuiCol_SliderGrabActive] = C(236, 170, 80);
    c[ImGuiCol_Button] = C(48, 38, 28, 235); c[ImGuiCol_ButtonHovered] = C(78, 58, 38, 245); c[ImGuiCol_ButtonActive] = C(100, 70, 40, 255);
    c[ImGuiCol_Header] = C(90, 60, 34, 200); c[ImGuiCol_HeaderHovered] = C(120, 76, 40, 220); c[ImGuiCol_HeaderActive] = C(150, 90, 44, 255);
    c[ImGuiCol_ScrollbarBg] = C(0, 0, 0, 40); c[ImGuiCol_ScrollbarGrab] = C(110, 84, 58, 200); c[ImGuiCol_ScrollbarGrabHovered] = C(140, 104, 66, 220); c[ImGuiCol_ScrollbarGrabActive] = C(170, 120, 70, 255);
    c[ImGuiCol_Separator] = C(110, 86, 60, 160); c[ImGuiCol_ModalWindowDimBg] = C(10, 6, 4, 170);
    st.WindowPadding = ImVec2(24, 20); st.FramePadding = ImVec2(12, 7); st.ItemSpacing = ImVec2(10, 9);
    st.FrameRounding = 2; st.GrabRounding = 2; st.ScrollbarSize = 12; st.WindowBorderSize = 0; st.ChildBorderSize = 0; st.FrameBorderSize = 1; st.PopupRounding = 3;
    st.ScaleAllSizes(s);
    ImGui::GetStyle() = st;
}

// ---------------------------------------------------------------- backdrop
void Backdrop() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    float W = sz.x, H = sz.y;
    // dusk sky: warm at the top right, dark earth at the bottom
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + W, p.y + H * 0.62f), IM_COL32(58, 34, 22, 255), IM_COL32(96, 52, 26, 255), IM_COL32(44, 28, 20, 255), IM_COL32(30, 22, 17, 255));
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + H * 0.62f), ImVec2(p.x + W, p.y + H), IM_COL32(30, 22, 17, 255), IM_COL32(44, 28, 20, 255), IM_COL32(18, 13, 10, 255), IM_COL32(18, 13, 10, 255));
    // low sun with glow, right side
    ImVec2 sun(p.x + W * 0.80f, p.y + H * 0.56f); float R = H * 0.13f;
    for (int i = 10; i >= 1; --i) dl->AddCircleFilled(sun, R * (1.0f + i * 0.28f), IM_COL32(230, 120, 40, 7 + i), 64);
    dl->AddCircleFilled(sun, R, IM_COL32(236, 150, 62, 70), 64);
    // horizon and the acacia silhouette
    float hy = p.y + H * 0.66f;
    dl->AddRectFilled(ImVec2(p.x, hy), ImVec2(p.x + W, p.y + H), IM_COL32(16, 11, 8, 150));
    ImU32 k = IM_COL32(14, 9, 6, 200);
    float tx = p.x + W * 0.70f, s = H / 700.0f;
    dl->AddQuadFilled(ImVec2(tx - 7 * s, hy), ImVec2(tx + 7 * s, hy), ImVec2(tx + 4 * s, hy - 120 * s), ImVec2(tx - 3 * s, hy - 120 * s), k);
    dl->AddLine(ImVec2(tx, hy - 100 * s), ImVec2(tx - 60 * s, hy - 160 * s), k, 9 * s);
    dl->AddLine(ImVec2(tx + 2 * s, hy - 104 * s), ImVec2(tx + 70 * s, hy - 165 * s), k, 9 * s);
    dl->AddLine(ImVec2(tx, hy - 110 * s), ImVec2(tx + 8 * s, hy - 168 * s), k, 7 * s);
    unsigned seed = 7;
    for (int i = 0; i < 70; ++i) {
        float x = tx - 140 * s + Rand(seed) * 280 * s, y = hy - 170 * s + Rand(seed) * 22 * s + std::fabs(x - tx) * 0.05f;
        float rx = (24 + Rand(seed) * 34) * s, ry = (6 + Rand(seed) * 7) * s;
        dl->AddEllipseFilled(ImVec2(x, y), ImVec2(rx, ry), k, 0, 20);
    }
    // faint topographic map contours across the page
    for (int c = 0; c < 9; ++c) {
        float base = p.y + H * (0.08f + c * 0.105f), ph = c * 1.7f;
        ImVec2 pts[64];
        for (int i = 0; i < 64; ++i) { float x = i / 63.0f; pts[i] = ImVec2(p.x + x * W, base + std::sin(x * 6.0f + ph) * 14 * s + std::sin(x * 13.0f + ph * 2) * 6 * s); }
        dl->AddPolyline(pts, 64, IM_COL32(210, 170, 120, 14), 0, 1.2f * s);
    }
    // dust specks and a vignette
    seed = 99;
    for (int i = 0; i < 260; ++i) { ImVec2 q(p.x + Rand(seed) * W, p.y + Rand(seed) * H); dl->AddRectFilled(q, ImVec2(q.x + 1.5f * s, q.y + 1.5f * s), IM_COL32(230, 200, 150, 10 + (int)(Rand(seed) * 18))); }
    float v = 90 * s;
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + v, p.y + H), IM_COL32(0, 0, 0, 120), 0, 0, IM_COL32(0, 0, 0, 120));
    dl->AddRectFilledMultiColor(ImVec2(p.x + W - v, p.y), ImVec2(p.x + W, p.y + H), 0, IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), 0);
}

// ---------------------------------------------------------------- painted tape strip
void PaintStrip(ImVec2 a, ImVec2 b, ImU32 col, unsigned seed) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = b.y - a.y, w = b.x - a.x;
    const int N = 24; ImVec2 top[N + 1], bot[N + 1];
    for (int i = 0; i <= N; ++i) {
        float x = a.x + w * i / N;
        top[i] = ImVec2(x, a.y + (Rand(seed) - 0.3f) * h * 0.18f);
        bot[i] = ImVec2(x, b.y - (Rand(seed) - 0.3f) * h * 0.18f);
    }
    // ragged ends
    top[0].x -= h * 0.15f; bot[0].x += h * 0.25f; top[N].x += h * 0.2f; bot[N].x -= h * 0.1f;
    for (int i = 0; i < N; ++i) dl->AddQuadFilled(top[i], top[i + 1], bot[i + 1], bot[i], col);
    // brush texture: thin darker streaks along the stroke
    ImU32 dark = (col & 0x00FFFFFF) | 0x28000000;
    for (int i = 0; i < 6; ++i) { float y = a.y + h * (0.2f + Rand(seed) * 0.6f); float x0 = a.x + w * Rand(seed) * 0.3f, x1 = b.x - w * Rand(seed) * 0.3f; dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), dark, 1.0f); }
}

// an acacia (umbrella thorn) silhouette standing on (x, y): forked trunk, a flat wide crown of layered tufts
// tufted flat crown centred (x, cy), half width cw
static void Crown(ImDrawList* dl, float x, float cy, float cw, float s, ImU32 k, unsigned& seed, int tufts) {
    for (int i = 0; i < tufts; ++i) {
        float u = Rand(seed) * 2 - 1, dome = 1 - u * u;
        float px = x + u * cw, py = cy - dome * 6 * s - Rand(seed) * 4 * s;
        float rx = (7 + Rand(seed) * 11) * s * (1.05f - std::fabs(u) * 0.3f) * (cw / (66 * s) * 0.5f + 0.5f), ry = (3.0f + Rand(seed) * 3.0f) * s;
        dl->AddEllipseFilled(ImVec2(px, py), ImVec2(rx, ry), k, 0, 16);
    }
    for (int i = 0; i < tufts * 3 / 4; ++i) {   // twig fringe along the top
        float u = Rand(seed) * 2 - 1, px = x + u * cw * 0.95f, py = cy - (1 - u * u) * 7 * s - 5 * s;
        dl->AddLine(ImVec2(px, py), ImVec2(px + (Rand(seed) - 0.5f) * 5 * s, py - (2 + Rand(seed) * 4) * s), k, 0.9f * s);
    }
}
// an acacia silhouette standing on (x, y). style 0: the wide umbrella thorn (one flat crown on a short bent trunk);
// style 1: a taller tree, trunk forking low into two leaning stems, each with its own flat crown at a different height
static void Acacia(ImDrawList* dl, float x, float y, float s, ImU32 k, unsigned seed, float lean, int style) {
    float th = 3.0f * s;
    if (style == 0) {
        float h = 44 * s;
        ImVec2 mid(x + lean * 4 * s, y - h * 0.5f), top(x + lean * 10 * s, y - h);
        dl->AddQuadFilled(ImVec2(x - th, y), ImVec2(x + th, y), ImVec2(mid.x + th * 0.8f, mid.y), ImVec2(mid.x - th * 0.8f, mid.y), k);
        dl->AddQuadFilled(ImVec2(mid.x - th * 0.8f, mid.y), ImVec2(mid.x + th * 0.8f, mid.y), ImVec2(top.x + th * 0.5f, top.y), ImVec2(top.x - th * 0.5f, top.y), k);
        float cy = y - h - 10 * s, cw = 66 * s, ox = lean * 10 * s;
        dl->AddLine(top, ImVec2(x + ox - cw * 0.62f, cy - 1 * s), k, 2.2f * s);
        dl->AddLine(top, ImVec2(x + ox + cw * 0.62f, cy - 2 * s), k, 2.2f * s);
        dl->AddLine(ImVec2(top.x, top.y + 5 * s), ImVec2(x + ox - cw * 0.20f, cy - 3 * s), k, 1.6f * s);
        dl->AddLine(ImVec2(top.x, top.y + 3 * s), ImVec2(x + ox + cw * 0.25f, cy - 3 * s), k, 1.6f * s);
        dl->AddLine(ImVec2(x - th * 0.2f + lean * 2 * s, y - h * 0.42f), ImVec2(x + ox - cw * 0.42f, y - h * 0.78f), k, 1.4f * s);
        Crown(dl, x + ox, cy, cw, s, k, seed, 44);
    } else {
        float fh = 22 * s;                                         // the fork
        ImVec2 fork(x + lean * 3 * s, y - fh);
        dl->AddQuadFilled(ImVec2(x - th * 1.1f, y), ImVec2(x + th * 1.1f, y), ImVec2(fork.x + th * 0.9f, fork.y), ImVec2(fork.x - th * 0.9f, fork.y), k);
        ImVec2 lt(x - 26 * s + lean * 6 * s, y - 62 * s), rt(x + 25 * s + lean * 12 * s, y - 48 * s);   // stem tops
        dl->AddQuadFilled(ImVec2(fork.x - th * 0.9f, fork.y), ImVec2(fork.x + th * 0.2f, fork.y), ImVec2(lt.x + th * 0.4f, lt.y), ImVec2(lt.x - th * 0.4f, lt.y), k);
        dl->AddQuadFilled(ImVec2(fork.x - th * 0.2f, fork.y), ImVec2(fork.x + th * 0.9f, fork.y), ImVec2(rt.x + th * 0.4f, rt.y), ImVec2(rt.x - th * 0.4f, rt.y), k);
        float lcy = lt.y - 6 * s, rcy = rt.y - 5 * s, lcw = 40 * s, rcw = 34 * s;
        dl->AddLine(lt, ImVec2(lt.x - lcw * 0.6f, lcy), k, 1.8f * s); dl->AddLine(lt, ImVec2(lt.x + lcw * 0.55f, lcy), k, 1.8f * s);
        dl->AddLine(rt, ImVec2(rt.x - rcw * 0.5f, rcy), k, 1.6f * s); dl->AddLine(rt, ImVec2(rt.x + rcw * 0.6f, rcy), k, 1.6f * s);
        dl->AddLine(ImVec2(fork.x + th * 0.5f, fork.y - 8 * s), ImVec2(x + 30 * s, y - 32 * s), k, 1.3f * s);   // a bare side branch
        Crown(dl, lt.x, lcy, lcw, s, k, seed, 30);
        Crown(dl, rt.x, rcy, rcw, s, k, seed, 26);
    }
}
// v223: a small savanna in the header between the title and the version: two groups of trees, the low sun between them
static void HeaderSavanna(ImDrawList* dl, float x0, float x1, float yTop, float yBase) {
    float w = x1 - x0, hgt = yBase - yTop;
    if (w < 200 || hgt < 40) return;
    float s = hgt / 110.0f * 1.3f;                                    // 30% bigger (v224)
    if (s * 560 > w) s = w / 560.0f;
    float cx = (x0 + x1) * 0.5f;
    { float half = 280 * s; if (cx - half > x0) x0 = cx - half; if (cx + half < x1) x1 = cx + half; w = x1 - x0; }
    ImVec2 sun(cx, yBase - 18 * s);                                   // the low sun in the middle
    for (int i = 6; i >= 1; --i) dl->AddCircleFilled(sun, 16 * s * (1 + i * 0.35f), IM_COL32(232, 128, 44, 6 + i * 2), 40);
    dl->AddCircleFilled(sun, 16 * s, IM_COL32(240, 160, 70, 90), 40);
    ImU32 k = IM_COL32(16, 10, 7, 225), farK = IM_COL32(40, 26, 17, 170);
    // ground: a soft ridge, fading out at both ends, with grass tufts
    ImVec2 pts[64];
    for (int i = 0; i < 64; ++i) { float t = i / 63.0f; pts[i] = ImVec2(x0 + t * w, yBase - std::sin(t * 3.14159f) * 4 * s + std::sin(t * 23) * 1.0f * s); }
    for (int i = 0; i < 63; ++i) {
        float t = i / 62.0f, a = std::sin(t * 3.14159f);
        dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, yBase + 4 * s), ImVec2(pts[i].x, yBase + 4 * s), IM_COL32(16, 10, 7, (int)(225 * a)));
    }
    unsigned seed = 31;
    for (int i = 0; i < 34; ++i) { float t = 0.08f + Rand(seed) * 0.84f, gx = x0 + t * w, gy = yBase - std::sin(t * 3.14159f) * 4 * s;
        dl->AddLine(ImVec2(gx, gy), ImVec2(gx + (Rand(seed) - 0.5f) * 4 * s, gy - (3 + Rand(seed) * 5) * s), k, 1.0f * s); }
    // left group: the wide umbrella thorns
    Acacia(dl, cx - 225 * s, yBase - 1 * s, 0.38f * s, farK, 9, -0.2f, 0);
    Acacia(dl, cx - 135 * s, yBase - 3 * s, 0.95f * s, k, 17, -0.25f, 0);
    Acacia(dl, cx - 60 * s, yBase - 3 * s, 0.55f * s, k, 23, 0.3f, 0);
    // right group: the tall forked trees
    Acacia(dl, cx + 70 * s, yBase - 3 * s, 0.62f * s, k, 41, -0.2f, 1);
    Acacia(dl, cx + 150 * s, yBase - 3 * s, 0.95f * s, k, 47, 0.25f, 1);
    Acacia(dl, cx + 230 * s, yBase - 1 * s, 0.40f * s, farK, 53, 0.1f, 1);
}
void TitleBlock(const char* title, const char* subtitle, const char* version) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_fonts.title);
    dl->AddText(ImVec2(c.x + 3, c.y + 3), IM_COL32(0, 0, 0, 160), title);
    dl->AddText(c, Sand(), title);
    float th = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.body);
    dl->AddText(ImVec2(c.x + 2, c.y + th + 2), Faded(), subtitle);
    float bh = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.small);
    ImVec2 vs = ImGui::CalcTextSize(version);
    dl->AddText(ImVec2(c.x + ImGui::GetContentRegionAvail().x - vs.x, c.y + 6), Faded(), version);
    ImGui::PopFont();
    {   // the savanna between the title text and the version
        ImGui::PushFont(g_fonts.title); float tw = ImGui::CalcTextSize(title).x; ImGui::PopFont();
        ImGui::PushFont(g_fonts.body); float sw = ImGui::CalcTextSize(subtitle).x; ImGui::PopFont();
        float gap = 40 * (th / 60.0f);
        float x0 = c.x + (tw > sw ? tw : sw) + gap, x1 = c.x + ImGui::GetContentRegionAvail().x - vs.x - gap;
        HeaderSavanna(dl, x0, x1, c.y + 2, c.y + th + bh);
    }
    ImGui::Dummy(ImVec2(10, th + bh + 8));
}
bool SidebarButton(const char* label, bool selected) {
    ImGui::PushFont(g_fonts.side);
    ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() * 1.9f;
    bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (selected) PaintStrip(c, ImVec2(c.x + w, c.y + h), IM_COL32(214, 180, 128, 235), (unsigned)(uintptr_t)label[0] * 31u + 5u);
    else if (hov) PaintStrip(c, ImVec2(c.x + w, c.y + h), IM_COL32(120, 86, 52, 120), 11u);
    ImGui::GetWindowDrawList()->AddText(ImVec2(c.x + h * 0.4f, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f), selected ? Ink() : Sand(), label);
    ImGui::PopFont();
    return pressed;
}
void PageHeader(const char* title, const char* subtitle) {
    ImGui::PushFont(g_fonts.page); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Sand())); ImGui::TextUnformatted(title); ImGui::PopStyleColor(); ImGui::PopFont();
    if (subtitle) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Faded())); ImGui::TextWrapped("%s", subtitle); ImGui::PopStyleColor(); }
    ImGui::Dummy(ImVec2(1, 4));
}
void Section(const char* name) {
    ImGui::Dummy(ImVec2(1, 4));
    ImGui::PushFont(g_fonts.head);
    ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() * 1.55f;
    unsigned seed = 2166136261u; for (const char* q = name; *q; ++q) seed = (seed ^ (unsigned char)*q) * 16777619u;
    PaintStrip(c, ImVec2(c.x + w, c.y + h), IM_COL32(204, 168, 112, 225), seed);
    ImGui::GetWindowDrawList()->AddText(ImVec2(c.x + h * 0.9f, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Ink(), name);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1, 2));
}
bool Button(const char* label, bool primary, bool enabled, float width) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.28f, 0.09f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.35f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.42f, 0.15f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.94f, 0.86f, 1));
    }
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.15f));
    ImGui::EndDisabled();
    if (primary) ImGui::PopStyleColor(4);
    return r && enabled;
}
bool Checkbox(const char* label, bool* v, const char* help) {
    bool r = ImGui::Checkbox(label, v);
    if (help) { ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); Wrapped(help, Faded()); ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); }
    return r;
}
void Wrapped(const char* text, ImU32 col) {
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextWrapped("%s", text);
    if (col) ImGui::PopStyleColor();
}
void Mark(char kind) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = ImGui::GetTextLineHeight(); ImVec2 c = ImGui::GetCursorScreenPos(); ImVec2 m(c.x + s * 0.5f, c.y + s * 0.5f); float r = s * 0.32f;
    ImU32 col = kind == '+' ? Good() : kind == '!' ? Warn() : Bad();
    dl->AddQuadFilled(ImVec2(m.x, m.y - r), ImVec2(m.x + r, m.y), ImVec2(m.x, m.y + r), ImVec2(m.x - r, m.y), col);
    ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine();
}
} // namespace theme
