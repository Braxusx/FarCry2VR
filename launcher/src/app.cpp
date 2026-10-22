// app.cpp - Far Cry 2 VR launcher: window, Direct3D 11 + Dear ImGui loop, pages.
// The launcher's design follows the Dishonored VR launcher (GingasVR, zlib licence); this is a separate
// implementation for Far Cry 2 with its own theme (theme.cpp).
#include "game.h"
#include "theme.h"
#include "payload_ids.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <thread>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace fc2 { int ElevatedMain(int argc, wchar_t** argv); }
namespace page { void Bindings(float uiScale, const char* turnText); }
using namespace fc2;

// ---------------------------------------------------------------- D3D
static ID3D11Device* g_dev = nullptr; static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr; static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static float g_scale = 1.0f; static bool g_fontsDirty = false;
static int g_framesPending = 3;

static void CreateRT() { ID3D11Texture2D* bb = nullptr; g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)); if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); } }
static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }; D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { sd.BufferCount = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx); }
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT(); return true;
}

// ---------------------------------------------------------------- app state
enum Page { PageOverview, PageSettings, PageOptional, PageBindings, PageCheats, PageHelp };
struct App {
    Page page = PageOverview;
    std::vector<Game> games; int gameIdx = -1;
    Game game; State state; bool running = false;
    // worker (install / uninstall)
    std::atomic<bool> busy{ false }; std::string busyText; std::mutex mx; bool haveReport = false; Report report; Report pending; bool pendingReady = false;
    bool confirmUninstall = false, deleteIni = false;
    std::string notice; ImU32 noticeCol = 0;
    LaunchOptions launch;
    // optional features (mirrors fc2vr.ini)
    bool optClimb = false, optNpc = false, optDot = true, optSteady = true, optFlash = false, optNoFilter = false, optNoRim = false;
} g_app;
static const Game& g_app_game_for_settings() { return g_app.game; }

// ---------------------------------------------------------------- settings (fc2vr.ini, edited as a draft and applied)
enum SType { SCheck, SSlider, SCombo };
struct SOpt { const char* label; const char* value; };
struct Setting {
    int tab; const char* section; const wchar_t* key; const char* label; const char* help; SType type;
    float mn, mx; const char* fmt; float shown;   // slider range, display format, display factor
    const SOpt* opts; int nopts; const char* def;
};
static const SOpt kUpscaler[] = { { "Off", "0" }, { "NVIDIA DLSS", "1" } };
static const SOpt kQuality[] = { { "DLAA (native)", "0" }, { "Ultra Quality", "1" }, { "Quality", "2" }, { "Balanced", "3" }, { "Performance", "4" }, { "Ultra Performance", "5" } };
static const float kQualityRatio[] = { 1.0f, 0.77f, 0.667f, 0.58f, 0.5f, 0.333f };
static const SOpt kPreset[] = { { "Transformer K", "11" }, { "Transformer J", "10" }, { "Transformer 2 M", "13" }, { "Transformer 2 L", "12" }, { "NVIDIA recommended per mode", "16" } };
static const SOpt kSnap[] = { { "Smooth turning", "0" }, { "Snap 30 degrees", "30" }, { "Snap 45 degrees", "45" }, { "Snap 60 degrees", "60" }, { "Snap 90 degrees", "90" } };
static const SOpt kCull[] = { { "Wide view, full detail (recommended)", "1" }, { "Wide view, less distant detail (fastest)", "0" }, { "Game's own camera (edges may pop in)", "2" }, { "Experimental", "3" } };
static const SOpt kShadowRes[] = { { "Game's own (2048)", "0" }, { "High (2720)", "2720" } };
static const SOpt kCross[] = { { "None", "0" }, { "Duplex", "1" }, { "Duplex with mil dots", "2" } };
static const Setting kSettings[] = {
    // Display
    { 0, "Picture", L"RenderScale", "Resolution scale (per side)", "Width and height relative to what the headset asks for. Higher is sharper and slower.", SSlider, 0.5f, 2.0f, "%.0f%%", 100, nullptr, 0, "1.0" },
    { 0, "Upscaling & anti-aliasing", L"Dlss", "Upscaler", "RTX graphics cards.", SCombo, 0, 0, nullptr, 0, kUpscaler, 2, "1" },
    { 0, "Upscaling & anti-aliasing", L"DlssQuality", "Upscaler quality", nullptr, SCombo, 0, 0, nullptr, 0, kQuality, 6, "0" },
    { 0, "Upscaling & anti-aliasing", L"DlssPreset", "DLSS preset", "K and J are the tested models. M suits Performance and L suits Ultra Performance; \"NVIDIA recommended\" picks K, M or L by mode.", SCombo, 0, 0, nullptr, 0, kPreset, 5, "11" },
    { 0, "Performance", L"ShareShadows", "Shared shadow maps", "The sun's shadows are drawn once and used by both eyes. A big saving in busy areas; looks the same.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 0, "Performance", L"Afw", "AFW (alternate frame warping)", "Renders one eye per frame and rebuilds the other: much lighter, with some artefacts in motion. Works with DLAA only (not the upscaling modes).", SCheck, 0, 0, nullptr, 0, nullptr, 0, "0" },
    { 0, "Performance", L"CullMode", "Culling", "How the game decides what's in view for both eyes.", SCombo, 0, 0, nullptr, 0, kCull, 4, "1" },
    { 0, "Shadows", L"ShadowRadialScale", "Sharp shadow reach", "How far around you sun shadows stay sharp (game: 20 m). Lower it for frame rate.", SSlider, 1.0f, 10.0f, "%.0f m", 20, nullptr, 0, "7.0" },
    { 0, "Shadows", L"ShadowMapSize", "Shadow resolution", nullptr, SCombo, 0, 0, nullptr, 0, kShadowRes, 2, "2720" },
    { 0, "Menus & HUD", L"HudScale", "Menu / prompt size", nullptr, SSlider, 0.5f, 1.2f, "%.0f%%", 100, nullptr, 0, "0.8" },
    { 0, "Menus & HUD", L"MenuDistance", "Menu distance", "Main menu and loading screens.", SSlider, 1.5f, 6.0f, "%.1f m", 1, nullptr, 0, "3.0" },
    { 0, "Menus & HUD", L"HideInteractionPrompts", "Hide interaction prompts", "No use / pick-up icons on the HUD (you use things with your hands).", SCheck, 0, 0, nullptr, 0, nullptr, 0, "0" },
    { 0, "Menus & HUD", L"HideHurtOverlay", "Hide the damage overlay", "No red borders or hit markers.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 0, "Menus & HUD", L"ScopeCrosshair", "Scope reticle", nullptr, SCombo, 0, 0, nullptr, 0, kCross, 3, "2" },
    { 0, "Menus & HUD", L"HeightOffset", "View height adjustment", "Added to your real height (negative = lower).", SSlider, -0.3f, 0.3f, "%+.2f m", 1, nullptr, 0, "0.0" },
    // Controls
    { 1, "Turning", L"SnapTurn", "Turning", "Right stick left / right.", SCombo, 0, 0, nullptr, 0, kSnap, 5, "0" },
    { 1, "Turning", L"TurnFullSpeed", "Full-speed smooth turning", "Any clear push turns at full speed; off = speed follows how far you push.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Turning", L"TurnStickPitch", "Right stick also tilts the view", "Normally your head does that.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "0" },
    { 1, "Weapons", L"ScopeNeedsGrip", "Sights only with two hands", "Iron sights / scope only while the left hand holds the weapon (no accidental zoom when reaching over the shoulder).", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Weapons", L"SteadyHands", "Steady hands", "No sprint sway and no pulling the gun in near walls.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Weapons", L"FreezeReload", "Still weapon while reloading", "The gun stays where you hold it during reload animations.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Weapons", L"NoIdleAnimation", "No weapon-inspect animation", nullptr, SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"ThrowArc", "Throwing arc", "Grip + trigger shows where a grenade or molotov will land.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"ThrowArcRight", "Right hand can throw too", "While the right grip is held with an empty hand, the trigger aims a throw.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"MacheteHits", "Machete swings hit", "Swinging the machete into an enemy lands a hit.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"ContactPush", "Hands push things", "Your hands and weapon shove loose objects and bodies they hit.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"GripInteract", "Empty grip also uses things", "Off: only X uses things; grips grab and climb.", SCheck, 0, 0, nullptr, 0, nullptr, 0, "0" },
    { 1, "Hands", L"HoldLostTracking", "Keep a hand where it was when tracking is lost", nullptr, SCheck, 0, 0, nullptr, 0, nullptr, 0, "1" },
    { 1, "Hands", L"ControllerRumble", "Controller rumble", nullptr, SSlider, 0.0f, 1.0f, "%.0f%%", 100, nullptr, 0, "1.0" },
};
static const int kNumSettings = (int)(sizeof kSettings / sizeof kSettings[0]);
struct SettingsDraft { std::wstring saved[64], draft[64]; bool loaded = false; int tab = 0; };
static SettingsDraft g_set;
static void LoadSettings() {
    for (int i = 0; i < kNumSettings; ++i) { std::wstring v = IniGet(g_app_game_for_settings(), kSettings[i].key, Widen(kSettings[i].def).c_str()); g_set.saved[i] = g_set.draft[i] = v; }
    g_set.loaded = true;
}
static bool SettingsDirty() { if (!g_set.loaded) return false; for (int i = 0; i < kNumSettings; ++i) if (g_set.saved[i] != g_set.draft[i]) return true; return false; }
static void ApplySettings() { for (int i = 0; i < kNumSettings; ++i) if (g_set.saved[i] != g_set.draft[i]) { IniSet(g_app_game_for_settings(), kSettings[i].key, g_set.draft[i]); g_set.saved[i] = g_set.draft[i]; } }

static void LoadPrefs() {
    LaunchOptions& o = g_app.launch;
    o.godMode = GetPref(L"Cheats", L"GodMode", L"0") == L"1"; o.unlimitedAmmo = GetPref(L"Cheats", L"UnlimitedAmmo", L"0") == L"1";
    o.noJam = GetPref(L"Cheats", L"NoGunJam", L"0") == L"1"; o.allWeapons = GetPref(L"Cheats", L"UnlockAllWeapons", L"0") == L"1";
    o.skipIntro = GetPref(L"Cheats", L"SkipIntroMovies", L"1") == L"1";
    o.coreLimit = _wtoi(GetPref(L"Launch", L"CoreLimit", L"16").c_str()); if (o.coreLimit < 0) o.coreLimit = 16;
}
static void SavePrefs() {
    LaunchOptions& o = g_app.launch;
    SetPref(L"Cheats", L"GodMode", o.godMode ? L"1" : L"0"); SetPref(L"Cheats", L"UnlimitedAmmo", o.unlimitedAmmo ? L"1" : L"0");
    SetPref(L"Cheats", L"NoGunJam", o.noJam ? L"1" : L"0"); SetPref(L"Cheats", L"UnlockAllWeapons", o.allWeapons ? L"1" : L"0");
    SetPref(L"Cheats", L"SkipIntroMovies", o.skipIntro ? L"1" : L"0");
}
static void ReadOptional() {
    if (!g_app.state.iniExists) return;
    g_app.optClimb = IniGet(g_app.game, L"Climbing", L"0") != L"0";
    g_app.optNpc = IniGet(g_app.game, L"NpcGrab", L"0") != L"0";
    g_app.optDot = IniGet(g_app.game, L"ReticleDot", L"1") != L"0";
    g_app.optSteady = IniGet(g_app.game, L"SteadyCamera", L"1") != L"0";
    g_app.optFlash = IniGet(g_app.game, L"Flashlight", L"0") != L"0";
    g_app.optNoFilter = IniGet(g_app.game, L"NoColorFilter", L"0") != L"0";
    g_app.optNoRim = IniGet(g_app.game, L"NoRimLight", L"0") != L"0";
}
static void SelectGame(const Game& g) {
    g_app.game = g; g_app.state = Check(g); g_set.loaded = false;
    if (g.valid) SetPref(L"Game", L"Directory", g.root);
    ReadOptional();
}
static void Rescan() {
    g_app.games = FindGames(); g_app.gameIdx = g_app.games.empty() ? -1 : 0;
    if (g_app.gameIdx >= 0) SelectGame(g_app.games[0]); else { g_app.game = Game{}; g_app.state = State{}; }
}
static void Refresh() { if (g_app.game.valid) { g_app.state = Check(g_app.game); ReadOptional(); } g_app.running = GameRunning(); }

static void StartOp(const char* op, bool deleteIni) {
    if (g_app.busy) return;
    g_app.busy = true; g_app.busyText = !strcmp(op, "install") ? "Installing Far Cry 2 VR..." : "Removing Far Cry 2 VR...";
    g_app.haveReport = false; g_app.notice.clear();
    Game g = g_app.game; std::string o = op;
    std::thread([g, o, deleteIni] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Report r = RunOperation(g, o.c_str(), deleteIni);
        { std::lock_guard<std::mutex> l(g_app.mx); g_app.pending = r; g_app.pendingReady = true; }
        g_app.busy = false;
        CoUninitialize();
    }).detach();
}
static void PollWorker() {
    std::lock_guard<std::mutex> l(g_app.mx);
    if (!g_app.pendingReady) return;
    g_app.pendingReady = false; g_app.report = g_app.pending; g_app.haveReport = true; g_set.loaded = false;
    Refresh(); g_framesPending = 3;
}

static void BrowseForGame(HWND hwnd) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); d->SetTitle(L"Select the Far Cry 2 folder");
    if (SUCCEEDED(d->Show(hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                Game g = Inspect(p);
                if (g.valid) { g_app.games.insert(g_app.games.begin(), g); g_app.gameIdx = 0; SelectGame(g); g_app.notice.clear(); }
                else { g_app.notice = "That folder doesn't contain Far Cry 2 (bin\\FarCry2.exe). Choose the game's main folder."; g_app.noticeCol = theme::Bad(); }
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}
static void DesktopShortcut() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PWSTR desk = nullptr; if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return;
    std::wstring lnk = std::wstring(desk) + L"\\Far Cry 2 VR.lnk"; CoTaskMemFree(desk);
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(self); sl->SetIconLocation(self, 0); sl->SetDescription(L"Far Cry 2 VR launcher");
        std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L'\\')); sl->SetWorkingDirectory(dir.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { bool ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); g_app.notice = ok ? "Desktop shortcut created." : "Couldn't create the shortcut."; g_app.noticeCol = ok ? theme::Good() : theme::Bad(); }
        sl->Release();
    }
}

// ---------------------------------------------------------------- pages
static const char* StoreName(Store s) { return s == StoreSteam ? "Steam" : s == StoreGog ? "GOG" : "other"; }
static void StatusLine(char kind, const char* text) { theme::Mark(kind); ImGui::TextWrapped("%s", text); }

static void PageOverviewDraw(HWND hwnd) {
    theme::PageHeader("Overview", "Install the VR mod into Far Cry 2 with one click, and remove it just as easily.");
    theme::Section("Game");
    if (!g_app.game.valid) {
        StatusLine('x', "Far Cry 2 wasn't found automatically. Use Browse to pick the game folder (the one that contains the bin folder).");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Faded())); ImGui::TextUnformatted("Location"); ImGui::PopStyleColor();
        ImGui::SameLine(110 * g_scale); ImGui::TextWrapped("%s", Narrow(g_app.game.root).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Faded())); ImGui::TextUnformatted("Edition"); ImGui::PopStyleColor();
        ImGui::SameLine(110 * g_scale);
        ImGui::Text("%s%s", StoreName(g_app.game.store), g_app.game.build == BuildUnknown ? "  (unrecognised game version: the mod supports Steam and GOG 1.03)" : "");
        if (g_app.games.size() > 1) {
            ImGui::SetNextItemWidth(-1);
            std::string cur = Narrow(g_app.game.root);
            if (ImGui::BeginCombo("##games", cur.c_str())) {
                for (int i = 0; i < (int)g_app.games.size(); ++i) { std::string n = Narrow(g_app.games[i].root) + "  (" + StoreName(g_app.games[i].store) + ")"; if (ImGui::Selectable(n.c_str(), i == g_app.gameIdx)) { g_app.gameIdx = i; SelectGame(g_app.games[i]); } }
                ImGui::EndCombo();
            }
        }
    }
    if (theme::Button("Browse...", false, !g_app.busy)) BrowseForGame(hwnd);
    ImGui::SameLine(); if (theme::Button("Search again", false, !g_app.busy)) Rescan();
    if (g_app.game.valid) { ImGui::SameLine(); if (theme::Button("Open game folder")) ShellExecuteW(nullptr, L"open", g_app.game.bin.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

    if (g_app.game.valid) {
        theme::Section("VR mod");
        const State& s = g_app.state;
        if (s.installed) {
            std::string t = "Installed" + (s.installedVersion.empty() ? std::string() : " (version " + s.installedVersion + ")") + ".";
            if (!s.installedVersion.empty() && s.installedVersion != kModVersion) t += std::string(" This launcher has version ") + kModVersion + ": Reinstall to update.";
            else if (s.outdated) t += " The installed mod files differ from the ones in this launcher; Reinstall / update installs this launcher's copy (your settings are kept).";
            StatusLine((s.installedVersion.empty() || s.installedVersion == kModVersion) && !s.outdated ? '+' : '!', t.c_str());
            if (!s.vrExe) StatusLine('!', "FarCry2VR.exe is missing: Reinstall to recreate it.");
        } else StatusLine('!', "Not installed.");
        if (s.foreignDinput) StatusLine('!', "Another mod's dinput8.dll is in the bin folder. Installing keeps it as a backup and puts it back on uninstall.");
        if (s.needsElevation) StatusLine('!', "The game folder needs administrator rights to change; Windows will ask once.");
        if (g_app.running) StatusLine('!', "Far Cry 2 is running. Close it to install or remove the mod.");
        ImGui::Dummy(ImVec2(1, 4));
        bool can = !g_app.busy && !g_app.running;
        if (theme::Button(s.installed ? "Reinstall / update" : "Install VR mod", !s.installed, can)) { g_app.confirmUninstall = false; StartOp("install", false); }
        if (s.installed) {
            ImGui::SameLine();
            if (theme::Button("Uninstall", false, can)) g_app.confirmUninstall = !g_app.confirmUninstall;
        }
        if (g_app.confirmUninstall && s.installed) {
            ImGui::Dummy(ImVec2(1, 2));
            ImGui::Indent(14 * g_scale);
            theme::Wrapped("Remove Far Cry 2 VR from this game? The game itself and your save games are not touched.", theme::Sand());
            ImGui::Checkbox("Also delete my VR settings (fc2vr.ini)", &g_app.deleteIni);
            if (theme::Button("Yes, uninstall", true, can)) { g_app.confirmUninstall = false; StartOp("uninstall", g_app.deleteIni); }
            ImGui::SameLine(); if (theme::Button("Cancel")) g_app.confirmUninstall = false;
            ImGui::Unindent(14 * g_scale);
        }
        theme::Section("Shortcuts");
        if (theme::Button("Create desktop shortcut")) DesktopShortcut();
        ImGui::SameLine(); theme::Wrapped("A shortcut to this launcher. Play starts the game in VR from here.", theme::Faded());
    }
    if (g_app.haveReport) {
        theme::Section(g_app.report.ok ? "Done" : "Something went wrong");
        for (auto& l : g_app.report.lines) if (l.size() > 2) StatusLine(l[0], l.c_str() + 2);
    }
}

static int SettingIndex(const wchar_t* key) { for (int i = 0; i < kNumSettings; ++i) if (!wcscmp(kSettings[i].key, key)) return i; return -1; }
static void SettingRow(int i) {
    const Setting& st = kSettings[i]; std::wstring& v = g_set.draft[i];
    // the upscaler's options only matter with DLSS on; AFW only with DLAA
    static const int iDlss = SettingIndex(L"Dlss"), iQual = SettingIndex(L"DlssQuality"), iScale = SettingIndex(L"RenderScale");
    const bool dlssOn = iDlss >= 0 && g_set.draft[iDlss] != L"0";
    const int qual = iQual >= 0 ? _wtoi(g_set.draft[iQual].c_str()) : 0;
    const bool disabled = (!dlssOn && (!wcscmp(st.key, L"DlssQuality") || !wcscmp(st.key, L"DlssPreset"))) || (!wcscmp(st.key, L"Afw") && dlssOn && qual > 0);
    if (disabled) ImGui::BeginDisabled();
    ImGui::PushID(i);
    float labelW = 230 * g_scale, ctrlW = ImGui::GetContentRegionAvail().x - labelW;
    if (st.type == SCheck) {
        bool on = v != L"0" && !v.empty();
        if (ImGui::Checkbox(st.label, &on)) v = on ? L"1" : L"0";
    } else {
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(st.label); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(ctrlW);
        if (st.type == SSlider) {
            float f = (float)_wtof(v.c_str()); float d = f * st.shown;
            if (ImGui::SliderFloat("##s", &d, st.mn * st.shown, st.mx * st.shown, st.fmt)) {
                f = d / st.shown; wchar_t b[32];
                swprintf(b, 32, st.shown >= 100 ? L"%.2f" : L"%.2f", f);
                std::wstring nv = b; while (nv.size() > 3 && nv.back() == L'0' && nv[nv.size() - 2] != L'.') nv.pop_back();
                v = nv;
            }
        } else {
            int cur = -1; for (int k = 0; k < st.nopts; ++k) if (Widen(st.opts[k].value) == v) cur = k;
            const char* preview = cur >= 0 ? st.opts[cur].label : "(custom value)";
            if (ImGui::BeginCombo("##c", preview)) { for (int k = 0; k < st.nopts; ++k) if (ImGui::Selectable(st.opts[k].label, k == cur)) v = Widen(st.opts[k].value); ImGui::EndCombo(); }
        }
    }
    if (st.help) { ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); theme::Wrapped(st.help, theme::Faded()); ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); }
    if (!wcscmp(st.key, L"DlssQuality") && dlssOn) {   // what the chosen mode means in pixels
        const float scale = iScale >= 0 ? (float)_wtof(g_set.draft[iScale].c_str()) : 1.0f, r = kQualityRatio[qual >= 0 && qual <= 5 ? qual : 0];
        char t[256];
        if (qual <= 0) snprintf(t, sizeof t, "Render quality above sets the headset picture. DLAA renders it at that size and smooths it (%.0f%% of the headset's pixels).", scale * scale * 100);
        else snprintf(t, sizeof t, "Render quality above sets the headset picture; this mode renders %.0f%% of its width and height (%.0f%% of the headset's pixels) and DLSS rebuilds full detail. Lower modes run faster and soften the picture. AFW is not available with upscaling.",
                      r * 100, scale * scale * r * r * 100);
        theme::Wrapped(t, theme::Faded());
    }
    if (!wcscmp(st.key, L"Afw") && disabled) theme::Wrapped("Set Upscaler quality to DLAA to use AFW.", theme::Warn());
    ImGui::PopID();
    if (disabled) ImGui::EndDisabled();
}
static void PageSettingsDraw() {
    theme::PageHeader("Settings", "Headset, picture and controls for the next launch. Changes are written to fc2vr.ini when you press Apply settings.");
    if (!g_app.state.iniExists) { theme::Wrapped("Install the VR mod first (Overview).", theme::Faded()); return; }
    if (!g_set.loaded) LoadSettings();
    const char* tabs[] = { "Display", "Controls" };
    for (int t = 0; t < 2; ++t) {
        if (t) ImGui::SameLine();
        bool sel = g_set.tab == t;
        if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.66f, 0.44f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.14f, 0.10f, 0.07f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.72f, 0.50f, 1)); }
        if (ImGui::Button(tabs[t], ImVec2(130 * g_scale, ImGui::GetFrameHeight() * 1.15f))) g_set.tab = t;
        if (sel) ImGui::PopStyleColor(3);
    }
    if (g_set.tab == 0) {   // render quality presets, as the Dishonored VR launcher has them
        theme::Section("Render quality");
        static const struct { const char* n; const char* sub; const wchar_t* v; } q[] = { { "Performance", "75% pixels", L"0.87" }, { "Balanced", "100% pixels", L"1.0" }, { "Quality", "120% pixels", L"1.1" }, { "Ultra", "150% pixels", L"1.22" } };
        float w = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
        for (int k = 0; k < 4; ++k) {
            if (k) ImGui::SameLine();
            bool sel = std::abs(_wtof(g_set.draft[0].c_str()) - _wtof(q[k].v)) < 0.005;
            if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.66f, 0.44f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.14f, 0.10f, 0.07f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.72f, 0.50f, 1)); }
            char lbl[64]; snprintf(lbl, sizeof lbl, "%s\n%s", q[k].n, q[k].sub);
            if (ImGui::Button(lbl, ImVec2(w, ImGui::GetFrameHeight() * 2.0f))) g_set.draft[0] = q[k].v;
            if (sel) ImGui::PopStyleColor(3);
        }
    }
    const char* cur = nullptr;
    for (int i = 0; i < kNumSettings; ++i) {
        if (kSettings[i].tab != g_set.tab) continue;
        if (!cur || strcmp(cur, kSettings[i].section)) { cur = kSettings[i].section; if (strcmp(cur, "Picture")) theme::Section(cur); }
        SettingRow(i);
    }
    ImGui::Dummy(ImVec2(1, 6));
    if (theme::Button("Reset this tab to defaults")) for (int i = 0; i < kNumSettings; ++i) if (kSettings[i].tab == g_set.tab) g_set.draft[i] = Widen(kSettings[i].def);
    ImGui::SameLine(); if (theme::Button("Open fc2vr.ini")) ShellExecuteW(nullptr, L"open", IniPath(g_app.game).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine(); theme::Wrapped("All other options are in fc2vr.ini.", theme::Faded());
}
static void PageOptionalDraw() {
    theme::PageHeader("Optional", "Extra VR features. Tick the ones you want; they take effect the next time the game starts.");
    if (!g_app.state.iniExists) { theme::Wrapped("Install the VR mod first (Overview).", theme::Faded()); return; }
    theme::Section("Features");
    bool changed = false;
    changed |= theme::Checkbox("VR Climbing", &g_app.optClimb, "Grab ledges, walls and ladders with your hands and pull yourself up.");
    changed |= theme::Checkbox("NPC Grabbing", &g_app.optNpc, "Grab people with a free hand: drag them, shove them or pull them to the ground.");
    changed |= theme::Checkbox("Dot reticle", &g_app.optDot, "A small aiming dot where the gun points, instead of the game's crosshair.");
    changed |= theme::Checkbox("Steady camera while running", &g_app.optSteady, "Removes the game's camera bob and sway while running and sprinting (more comfortable in VR).");
    changed |= theme::Checkbox("Remove the brown filter", &g_app.optNoFilter, "Turns off the game's colour grading and desaturation, for natural colours.");
    changed |= theme::Checkbox("Remove rim lighting", &g_app.optNoRim, "Turns off the bright edge lighting on characters' skin and clothes.");
    changed |= theme::Checkbox("Flashlight", &g_app.optFlash, "A head-mounted light. Raise your left hand to your left temple and pull the left trigger to switch it on or off.");
    if (changed) {
        IniSet(g_app.game, L"Climbing", g_app.optClimb ? L"1" : L"0");
        IniSet(g_app.game, L"NpcGrab", g_app.optNpc ? L"1" : L"0");
        IniSet(g_app.game, L"ReticleDot", g_app.optDot ? L"1" : L"0");
        IniSet(g_app.game, L"SteadyCamera", g_app.optSteady ? L"1" : L"0");
        IniSet(g_app.game, L"Flashlight", g_app.optFlash ? L"1" : L"0");
        IniSet(g_app.game, L"NoColorFilter", g_app.optNoFilter ? L"1" : L"0");
        IniSet(g_app.game, L"NoRimLight", g_app.optNoRim ? L"1" : L"0");
        g_app.notice = "Saved. Takes effect the next time the game starts."; g_app.noticeCol = theme::Good();
    }
}
static void PageBindingsDraw() {
    std::wstring snap = g_app.state.iniExists ? IniGet(g_app.game, L"SnapTurn", L"0") : L"0";
    char turn[96]; int deg = _wtoi(snap.c_str());
    if (deg > 0) snprintf(turn, sizeof turn, "Turn (snap %d degrees).", deg); else snprintf(turn, sizeof turn, "Turn (smooth).");
    page::Bindings(g_scale, turn);
}
static void PageCheatsDraw() {
    theme::PageHeader("Cheats", "The game's own cheat switches, applied when you press Play.");
    theme::Section("Cheats");
    LaunchOptions& o = g_app.launch; bool c = false;
    c |= theme::Checkbox("God Mode", &o.godMode, "You can't be killed.");
    c |= theme::Checkbox("Unlimited Ammo", &o.unlimitedAmmo, "Magazines never run out.");
    c |= theme::Checkbox("No Gun Jam", &o.noJam, "Weapons never wear out, so they never jam or break.");
    c |= theme::Checkbox("Unlock All Weapons", &o.allWeapons, "Every weapon is available at the gun shops (depending on which part of the map you're in).");
    theme::Section("Startup");
    c |= theme::Checkbox("Skip intro movies", &o.skipIntro, "Go straight to the main menu.");
    if (c) SavePrefs();
}
static void PageHelpDraw() {
    theme::PageHeader("Help & about", nullptr);
    theme::Section("Need help?");
    theme::Wrapped("If the game crashes, freezes or something doesn't work right, collect the logs and send the zip with your report. It gathers the mod's logs, settings and a short system summary (Windows, CPU, GPU, VR runtime) into one zip on your desktop. Nothing is uploaded.", theme::Faded());
    if (theme::Button("Collect logs", true)) {
        std::string err; std::wstring z = CollectLogs(g_app.game, err);
        if (!z.empty()) { g_app.notice = "Saved to your desktop: " + Narrow(z.substr(z.find_last_of(L'\\') + 1)); g_app.noticeCol = theme::Good(); std::wstring arg = L"/select,\"" + z + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL); }
        else { g_app.notice = err; g_app.noticeCol = theme::Bad(); }
    }
    ImGui::SameLine();
    if (theme::Button("Open launcher log")) { std::wstring p = PrefsDir() + L"\\launcher.log"; ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    theme::Section("Credits");
    ImGui::PushFont(theme::g_fonts.head); ImGui::TextUnformatted("Far Cry VR Mod by Braxus"); ImGui::PopFont();
    ImGui::Text("Version %s", kModVersion);
    ImGui::Dummy(ImVec2(1, 6));
    theme::Wrapped("Launcher design modelled on the Dishonored VR launcher by Pizza Parker (github.com/VR-Stereo-Hub/Dishonored-VR), used with his permission. Dishonored VR was originally created by GingasVR. This is a separate implementation for Far Cry 2.", theme::Faded());
    theme::Wrapped("Dear ImGui by Omar Cornut (MIT licence). OpenXR loader by the Khronos Group (Apache 2.0). NVIDIA DLSS (NVIDIA RTX SDK licence).", theme::Faded());
    theme::Wrapped("Far Cry is a trademark of Ubisoft Entertainment. This is an unofficial fan mod, not affiliated with or endorsed by Ubisoft.", theme::Faded());
}

static void DrawUI(HWND hwnd) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    theme::Backdrop();
    char ver[48]; snprintf(ver, sizeof ver, "Far Cry 2 VR %s", kModVersion);
    theme::TitleBlock("FAR CRY 2 VR", "Motion-controlled virtual reality for Far Cry 2", ver);
    float footerH = 64 * g_scale, sideW = 190 * g_scale;
    float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    // sidebar
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(1, 8 * g_scale));
    static const char* names[] = { "Overview", "Settings", "Optional", "Bindings", "Cheats", "Help & about" };
    for (int i = 0; i < 6; ++i) if (theme::SidebarButton(names[i], g_app.page == i)) { g_app.page = (Page)i; g_app.notice.clear(); }
    // status under the navigation
    ImGui::SetCursorPosY(bodyH - ImGui::GetTextLineHeightWithSpacing() * 3.3f);
    ImGui::PushFont(theme::g_fonts.small); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Faded()));
    if (g_app.game.valid) { ImGui::Text("Far Cry 2 - %s", StoreName(g_app.game.store)); ImGui::TextUnformatted(g_app.state.installed ? "VR installed" : "VR not installed"); ImGui::TextUnformatted(g_app.running ? "Game running" : "Settings apply at launch"); }
    else ImGui::TextUnformatted("Game not found");
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::EndChild();
    ImGui::SameLine();
    { ImVec2 c = ImGui::GetCursorScreenPos(); ImGui::GetWindowDrawList()->AddLine(ImVec2(c.x, c.y + 6), ImVec2(c.x, c.y + bodyH - 6), IM_COL32(150, 110, 60, 120), 1.5f * g_scale); ImGui::Dummy(ImVec2(14 * g_scale, 1)); ImGui::SameLine(); }
    // page (darkened panel for legibility over the backdrop)
    ImVec2 pc = ImGui::GetCursorScreenPos(); float pw = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(pc, ImVec2(pc.x + pw, pc.y + bodyH), IM_COL32(16, 11, 8, 150), 4 * g_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * g_scale, 14 * g_scale));
    ImGui::BeginChild("##page", ImVec2(pw, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (g_app.busy) { theme::PageHeader("Please wait", nullptr); ImGui::TextUnformatted(g_app.busyText.c_str()); float t = (float)ImGui::GetTime(); ImVec2 c = ImGui::GetCursorScreenPos(); float r = 12 * g_scale; ImGui::GetWindowDrawList()->PathArcTo(ImVec2(c.x + r, c.y + r + 6), r, t * 5, t * 5 + 4.2f, 24); ImGui::GetWindowDrawList()->PathStroke(theme::Ochre(), 0, 3 * g_scale); g_framesPending = 2; }
    else switch (g_app.page) {
        case PageOverview: PageOverviewDraw(hwnd); break;
        case PageSettings: PageSettingsDraw(); break;
        case PageOptional: PageOptionalDraw(); break;
        case PageBindings: PageBindingsDraw(); break;
        case PageCheats: PageCheatsDraw(); break;
        case PageHelp: PageHelpDraw(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // footer: notice on the left, Close and Play on the right
    ImGui::Dummy(ImVec2(1, 8 * g_scale));
    float bw = 130 * g_scale, by = ImGui::GetCursorPosY();
    if (!g_app.notice.empty()) { ImGui::SetCursorPosY(by + 8 * g_scale); ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - bw * 2 - 40 * g_scale); theme::Wrapped(g_app.notice.c_str(), g_app.noticeCol ? g_app.noticeCol : theme::Faded()); ImGui::PopTextWrapPos(); }
    float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SetCursorPos(ImVec2(right - bw * 2 - 10 * g_scale, by));
    bool dirty = SettingsDirty();
    if (dirty) {
        ImGui::SetCursorPos(ImVec2(right - bw * 3.4f - 20 * g_scale, by));
        if (theme::Button("Apply settings", false, true, bw * 1.4f)) { ApplySettings(); g_app.notice = "Settings saved. They apply the next time the game starts."; g_app.noticeCol = theme::Good(); }
        ImGui::SameLine();
        if (g_app.notice.empty()) { g_app.notice = "Unsaved settings. Apply before playing."; g_app.noticeCol = theme::Warn(); }
    } else if (g_app.notice == "Unsaved settings. Apply before playing.") g_app.notice.clear();
    ImGui::SetCursorPos(ImVec2(right - bw * 2 - 10 * g_scale, by));
    if (theme::Button("Close", false, true, bw)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ImGui::SameLine();
    bool canPlay = g_app.game.valid && g_app.state.installed && !g_app.busy && !g_app.running;
    bool doPlay = false;
    if (theme::Button(g_app.running ? "Running" : "Play", true, canPlay, bw)) { if (dirty) ImGui::OpenPopup("Unsaved settings"); else doPlay = true; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("You changed settings without applying them.");
        if (theme::Button("Apply and play", true)) { ApplySettings(); doPlay = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (theme::Button("Play with saved settings")) { for (int i = 0; i < kNumSettings; ++i) g_set.draft[i] = g_set.saved[i]; doPlay = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (theme::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (doPlay) {
        std::string err;
        if (Launch(g_app.game, g_app.launch, err)) { g_app.notice = "Starting Far Cry 2 VR..."; g_app.noticeCol = theme::Good(); g_app.running = true; }
        else { g_app.notice = err; g_app.noticeCol = theme::Bad(); }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) { g_framesPending = 3; return 1; }
    switch (msg) {
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); } g_framesPending = 3; return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* m = (MINMAXINFO*)lp; m->ptMinTrackSize = { (LONG)(820 * g_scale), (LONG)(600 * g_scale) }; return 0; }
    case WM_DPICHANGED: { g_scale = HIWORD(wp) / 96.0f; g_fontsDirty = true; RECT* r = (RECT*)lp; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) g_framesPending = 3;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--op")) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); int r = ElevatedMain(argc, argv); CoUninitialize(); return r; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("launcher %s started", kModVersion);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FarCry2VR.Launcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindowW(L"FarCry2VRLauncher", nullptr); if (o) { ShowWindow(o, SW_RESTORE); SetForegroundWindow(o); } return 0; }

    WNDCLASSEXW wc{ sizeof wc }; wc.style = CS_CLASSDC; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"FarCry2VRLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    POINT pt; GetCursorPos(&pt); HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForSystem(); { typedef HRESULT (WINAPI *GDFM)(HMONITOR, int, UINT*, UINT*); auto f = (GDFM)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"); UINT y; if (f) f(mon, 0, &dpi, &y); }
    g_scale = dpi / 96.0f;
    RECT r{ 0, 0, (LONG)(1000 * g_scale), (LONG)(720 * g_scale) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Far Cry 2 VR", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    { BOOL dark = TRUE; typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD); auto f = (DSWA)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"); if (f) f(hwnd, 20, &dark, sizeof dark); }
    if (!CreateDevice(hwnd)) { MessageBoxW(hwnd, L"Direct3D 11 isn't available on this PC, so the launcher can't open.", L"Far Cry 2 VR", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, show); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);

    LoadPrefs(); Rescan(); g_app.running = GameRunning();
    double nextPoll = 0;
    bool done = false;
    while (!done) {
        DWORD wait = (g_framesPending > 0 || g_app.busy) ? 16 : 1000;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(hwnd)) continue;
        if (GetTickCount64() > nextPoll) { bool run = GameRunning(); if (run != g_app.running) { g_app.running = run; if (!run) Refresh(); g_framesPending = 3; } nextPoll = GetTickCount64() + 1000.0; }
        PollWorker();
        if (g_resizeW) { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRT(); }
        if (g_fontsDirty) { g_fontsDirty = false; ImGui_ImplDX11_InvalidateDeviceObjects(); theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale); ImGui_ImplDX11_CreateDeviceObjects(); }
        if (g_framesPending <= 0 && !g_app.busy) continue;
        --g_framesPending;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawUI(hwnd);
        ImGui::Render();
        const float clear[4] = { 0.07f, 0.05f, 0.04f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release(); if (g_swap) g_swap->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    if (single) CloseHandle(single);
    CoUninitialize();
    return 0;
}
