// bindings.cpp - the Bindings page: both Quest 3 controllers drawn in code with every button labelled (on foot),
// and cards for holsters, motion, vehicles and keyboard keys. The bindings follow controls.cpp of the mod.
#include "theme.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace page {

using namespace theme;

// one labelled control: anchor on the controller (canvas units) and where its text goes

static void Controller(ImDrawList* dl, ImVec2 o, float s, bool left) {
    // o = centre of the controller's face (screen), s = scale; mirrored for the left hand
    float m = left ? 1.0f : -1.0f;
    auto P = [&](float x, float y) { return ImVec2(o.x + x * m * s, o.y + y * s); };
    ImU32 shell = IM_COL32(222, 214, 198, 255), shade = IM_COL32(176, 166, 148, 255), face = IM_COL32(44, 38, 34, 255), dark = IM_COL32(22, 19, 17, 255);
    // handle: from under the face, down and towards the inside
    ImVec2 h[] = { P(-30, 40), P(28, 52), P(46, 120), P(44, 196), P(26, 218), P(4, 214), P(-8, 190), P(-18, 120) };
    dl->AddConvexPolyFilled(h, 8, shade);
    ImVec2 hi[] = { P(-24, 46), P(22, 56), P(38, 120), P(36, 190), P(22, 206), P(8, 204), P(-2, 186), P(-12, 120) };
    dl->AddConvexPolyFilled(hi, 8, shell);
    // grip button on the handle's inner side
    dl->AddRectFilled(P(left ? 30 : 44, 112), P(left ? 44 : 30, 150), IM_COL32(150, 140, 124, 255), 5 * s);
    // trigger: a tab peeking out above the face, front side
    dl->AddRectFilled(P(left ? 18 : 46, -96), P(left ? 46 : 18, -78), shade, 6 * s);
    // face: an oval tilted a little
    dl->AddEllipseFilled(o, ImVec2(92 * s, 78 * s), shell, left ? -0.25f : 0.25f, 48);
    dl->AddEllipseFilled(o, ImVec2(80 * s, 66 * s), face, left ? -0.25f : 0.25f, 48);
    // thumbstick
    ImVec2 st = P(-30, -12);
    dl->AddCircleFilled(st, 24 * s, dark, 32); dl->AddCircleFilled(st, 17 * s, IM_COL32(60, 54, 50, 255), 32); dl->AddCircle(st, 17 * s, IM_COL32(90, 82, 76, 255), 32, 1.5f * s);
    // face buttons: upper (Y / B) and lower (X / A); small system button below
    ImVec2 b1 = P(30, -26), b2 = P(16, 16);
    for (ImVec2 b : { b1, b2 }) { dl->AddCircleFilled(b, 15 * s, dark, 28); dl->AddCircle(b, 15 * s, IM_COL32(120, 110, 98, 255), 28, 1.2f * s); }
    ImGui::PushFont(g_fonts.small);
    auto label = [&](ImVec2 c, const char* t) { ImVec2 ts = ImGui::CalcTextSize(t); dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), Sand(), t); };
    label(b1, left ? "Y" : "B"); label(b2, left ? "X" : "A");
    ImGui::PopFont();
    ImVec2 sys = P(-14, 40);
    dl->AddCircleFilled(sys, 9 * s, dark, 20);
    if (left) for (int i = -1; i <= 1; ++i) dl->AddLine(ImVec2(sys.x - 4 * s, sys.y + i * 3 * s), ImVec2(sys.x + 4 * s, sys.y + i * 3 * s), Sand(), 1.2f * s);
    else { dl->AddCircle(ImVec2(sys.x - 2.5f * s, sys.y), 2.8f * s, Sand(), 12, 1.2f * s); dl->AddCircle(ImVec2(sys.x + 2.5f * s, sys.y), 2.8f * s, Sand(), 12, 1.2f * s); }
}

// callout: name + text block at (tx, ty) (screen), leader line from the anchor to the block's inner edge
static float DrawCallout(ImDrawList* dl, ImVec2 anchor, float tx, float ty, float textW, bool right, const char* name, const char* text, float s, bool measureOnly) {
    ImGui::PushFont(g_fonts.head); float hh = ImGui::GetTextLineHeight(); ImGui::PopFont();
    ImGui::PushFont(g_fonts.small); ImVec2 bs = ImGui::CalcTextSize(text, nullptr, false, textW); ImGui::PopFont();
    float h = hh + bs.y;
    if (measureOnly) return h;
    ImU32 line = IM_COL32(210, 154, 58, 230);
    ImVec2 elbow(right ? tx - 6 * s : tx + textW + 6 * s, ty + hh * 0.55f);
    ImVec2 knee(right ? elbow.x - 14 * s : elbow.x + 14 * s, elbow.y);
    dl->AddLine(anchor, knee, line, 1.4f); dl->AddLine(knee, elbow, line, 1.4f);
    dl->AddCircleFilled(anchor, 3.0f * s + 1.0f, line, 12);
    ImGui::PushFont(g_fonts.head);
    float nx = right ? tx : tx + textW - ImGui::CalcTextSize(name).x;
    dl->AddText(ImVec2(nx, ty), Ochre(), name);
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.small);
    dl->AddText(nullptr, 0.0f, ImVec2(tx, ty + hh), Sand(), text, nullptr, textW);
    ImGui::PopFont();
    return h;
}

static void Card(const char* title, const char* const* lines, int n, float w) {
    ImGui::BeginGroup();
    ImGui::PushItemWidth(w);
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_fonts.head);
    float h = ImGui::GetTextLineHeight() * 1.5f;
    unsigned seed = 0x9E3779B9u; for (const char* q = title; *q; ++q) seed = (seed ^ (unsigned char)*q) * 16777619u;
    PaintStrip(c, ImVec2(c.x + w, c.y + h), IM_COL32(204, 168, 112, 225), seed);
    ImGui::GetWindowDrawList()->AddText(ImVec2(c.x + h * 0.6f, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Ink(), title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, h + 4));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    for (int i = 0; i < n; ++i) { Mark('+'); ImGui::TextWrapped("%s", lines[i]); }
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void Bindings(float uiScale, const char* turnText) {
    PageHeader("Bindings", "Quest 3 controllers in Far Cry 2 VR, on foot. Vehicles and keyboard keys are below.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_fonts.page);
    const char* title = "QUEST 3  -  CONTROLLER FIELD GUIDE";
    ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(o.x + (avail - ts.x) * 0.5f, o.y), Sand(), title);
    ImGui::PopFont();
    float top = o.y + ts.y + 14 * uiScale;
    // controllers: drawn at scale cs, faces at 38% / 62% of the width; text columns on the outside
    float cs = avail / 1050.0f; if (cs > 0.95f * uiScale) cs = 0.95f * uiScale;
    float textW = avail * 0.5f - 0.12f * avail - 100 * cs - 16 * uiScale; if (textW < 120 * uiScale) textW = 120 * uiScale;
    ImVec2 lc(o.x + avail * 0.38f, top + 110 * cs), rc(o.x + avail * 0.62f, top + 110 * cs);
    Controller(dl, lc, cs, true); Controller(dl, rc, cs, false);
    auto LP = [&](float x, float y) { return ImVec2(lc.x + x * cs, lc.y + y * cs); };
    auto RP = [&](float x, float y) { return ImVec2(rc.x - x * cs, rc.y + y * cs); };
    struct C { ImVec2 a; const char* n; const char* t; };
    static char rstick[160]; snprintf(rstick, sizeof rstick, "%s Pull down: crouch. Click: syrette (heal).", turnText);
    C left[] = {
        { LP(32, -87), "LEFT TRIGGER", "Reload / clear a jam. Right grip held: throw the grenade. At your left temple: flashlight on / off (if enabled)." },
        { LP(-30, -12), "LEFT STICK", "Move. Click: sprint. Hold in (1 s): recenter." },
        { LP(30, -26), "Y", "Map / GPS. Also answers the phone." },
        { LP(16, 16), "X", "Use: doors, turrets, pick-ups, vehicles." },
        { LP(-14, 40), "MENU", "Pause menu." },
        { LP(37, 131), "LEFT GRIP", "Two-handed hold, grab things, climb, grab people. Empty hand: grip + trigger aims a throw." },
    };
    C right[] = {
        { RP(32, -87), "RIGHT TRIGGER", "Fire (machete: swing it instead). Empty hand, grip held: throw." },
        { RP(-30, -12), "RIGHT STICK", rstick },
        { RP(30, -26), "B", "Tap: next weapon. Hold: grenade / molotov. Left grip + B: weapon away / back." },
        { RP(16, 16), "A", "Jump. Left grip + A: re-take the weapon's hold." },
        { RP(-14, 40), "META", "Quest system menu." },
        { RP(37, 131), "RIGHT GRIP", "At a holster spot: draw that weapon (below). Gun up to your eye: sights / scope." },
    };
    float gap = 10 * uiScale, yl = top, yr = top, lx = o.x, rx = o.x + avail - textW;
    for (auto& c : left)  yl += DrawCallout(dl, c.a, lx, yl, textW, false, c.n, c.t, cs, false) + gap;
    for (auto& c : right) yr += DrawCallout(dl, c.a, rx, yr, textW, true, c.n, c.t, cs, false) + gap;
    float bottom = yl > yr ? yl : yr; float ctl = lc.y + 230 * cs; if (ctl > bottom) bottom = ctl;
    ImGui::Dummy(ImVec2(avail, bottom - o.y + 8 * uiScale));

    // cards
    float cg = 16 * uiScale, cw = (avail - cg) * 0.5f;
    static const char* holsters[] = {
        "Over your right shoulder: the rifle slot; grab again for the special slot (sniper, RPG...).",
        "At your right hip, hand pointing down: the pistol slot.",
        "In front of your chest: the machete.",
        "Bring the weapon up to your eye to aim down the sights or scope (left hand on the weapon).",
    };
    static const char* motion[] = {
        "Swing the machete to attack: a fast swing whooshes, and hitting an enemy lands the blow (the trigger does nothing with the machete).",
        "Grip a ledge or wall and pull yourself up (Optional page: VR Climbing).",
        "Grab people with a free hand: drag, shove, or pull them down (Optional page: NPC Grabbing).",
        "Hold a grip, then the trigger, to aim a throw: an arc shows where it lands; let go to throw.",
    };
    static const char* vehicles[] = {
        "Right trigger: accelerate. Left trigger: brake / reverse. Left stick (or right): steer.",
        "Left grip: handbrake. Right grip or A: change seat. X: get out.",
        "Y: map. B: headlights. Right stick click: syrette.",
    };
    static const char* keys[] = {
        "F8: recenter the view (or hold the left stick in).",
        "End: hand-angle menu (machete, map, free hand, phone).",
        "Home: smooth turning speed (Left / Right arrows).",
        "F9: stereo on/off. DLSS, AFW, shadows: Settings page.",
    };
    Card("Weapons & holsters", holsters, 4, cw); ImGui::SameLine(0, cg); Card("Hands & motion", motion, 4, cw);
    ImGui::Dummy(ImVec2(1, 10 * uiScale));
    Card("Vehicles", vehicles, 3, cw); ImGui::SameLine(0, cg); Card("Keyboard (on the PC)", keys, 4, cw);
}

} // namespace page
