// main.cpp - Far Cry 2 VR (Steam v1.03, Dunia.dll SHA256 6353936a...862765, DX10 renderer)
//
// Loaded as a dinput8.dll proxy from FarCry2\bin. Stereo is produced inside the engine:
// CSceneRenderer::RenderFrame calls RenderViewport (vtbl[4], 0x1033F6F0) once per 3D viewport;
// we intercept the main-loop call (return address 0x1034626A) and run the original once per eye
// with a per-eye render camera (viewport+0x3F0). After each pass the backbuffer holds that eye's
// finished, post-processed 3D image (the HUD is drawn later by Render2DView), which is copied
// into a shared texture and submitted to OpenXR.
#include <vector>
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "xr.h"
#include "controls.h"
#include "arms.h"
#include "log.h"
#include "diag.h"
#include "gameaddr.h"
#include "perf.h"
#include "hang.h"
#include "dlss_fc2.h"
namespace sounddiag { void Tick(); }
#include "afw_fc2.h"
#include "common/pose_frame.h"

// ============================================================ logging
static CRITICAL_SECTION g_logCs;
static FILE* g_log = nullptr;
static char g_dir[MAX_PATH] = "";

void LogWrite(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

// 32-bit address space report: total/available virtual memory and the largest free block.
static void LogVA(const char* tag) {
    MEMORYSTATUSEX ms{ sizeof(ms) }; GlobalMemoryStatusEx(&ms);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000, largest = 0;
    while (a < 0xFFFF0000u && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break; a = next;
    }
    LogWrite("VA[%s]: total=%llu MB avail=%llu MB largestFree=%u MB", tag,
             ms.ullTotalVirtual >> 20, ms.ullAvailVirtual >> 20, (unsigned)(largest >> 20));
}

// ============================================================ config (fc2vr.ini next to the DLL)
struct Config {
    int   enable = 1;           // 0 = load but never touch rendering
    float worldScale = 1.0f;    // game units per metre
    float cullMargin = 1.25f;   // culling-frustum enlargement over the union of both eye FOVs
    int   adaptiveDetail = 1;     // cull mode 1/3: widen the detail (LOD) FOV toward the VR FOV while the CPU misses the frame budget
    float adaptiveTarget = 10.0f; // ms of main-thread work per frame to stay under
    float adaptiveMax = 1.0f;     // how far toward the VR FOV it may go (0..1)
    float renderScale = 1.0f;   // eye resolution relative to the headset's recommended size
    int steadyCam = 1;          // SteadyCamera: no camera bob / sway while running (v209)
    float monoForward = 0.30f;
    float flashForward = 0.10f;   // FlashlightForward (v220)  // MonocularForward: the monocular view starts this far in front of the eye (past the hand)
    int   dpiAware = 1;           // see DllMain
    float heightAdapt = 15.0f;    // seconds for the height reference to follow sustained posture (0 = fixed at recenter)
    float heightOffset = 0.0f;
    int   fixNearPass = 1;
    int   hudLayer = 1;           // HUD/menus as a separate quad layer
    float hudDistance = 3.0f;     // metres
    float hudFov = 60.0f;         // angular width of the HUD quad, degrees        // weapon/arms near pass uses the eye's off-axis projection
    float menuDistance = 3.0f;    // metres to the menu/loading screen
    float menuWidth = 2.4f;       // metres
    int   uiMode = 1;             // 1 = per-eye UI viewport: centred on the gun's aim point, scaled (fixes doubled reticle/menus)
    float uiDepth = 6.0f;         // metres: distance at which the UI converges (aim point)
    float uiScale = 0.7f;         // UI size relative to the eye image
    char  uiSkipTex[512] = "256x512x71v18";   // UI textures (WxHxFormat, comma-separated) not drawn into the layer
    int   reticleTexMax = 256;      // UI draws whose texture has at most this many texels go to the aim-locked reticle layer
    int   fixBouncingNpcs = 1, npcFixThreads = 1, npcFixJobThreads = 1;
    int   scopeMode = 1;              // flat screen for binoculars / scopes
    float scopeFovRatio = 0.6f;       // zoom is detected when the camera FOV drops below this fraction of normal
    float scopeDistance = 1.5f, scopeWidth = 1.2f;   // metres
    int   scopeFollowHead = 0;        // 0 = screen stays where you looked when zooming, 1 = follows the head
    int   devConsole = 1;             // 1 = "devmodeon" console command works, 2 = dev mode on automatically
    int   symmetricFov = 1;          // render each eye with a symmetric frustum (fixes water reflection streaks)
    float overscanH = 0.15f, overscanV = 0.25f;
    int   shadowFix = 1;
    float shadowFarScale = 3.0f;   // last sun shadow cascade range x this (1 = game)
    float shadowRadial = 7.0f;     // sun shadow cascades centered on the eye, radius = range x this (0 = game's fit)
    float grabDistance = 0.15f;
    int   scopeGameReticle = 1; float scopeAmmoSize = 0.28f, scopeAmmoX = 0.59f, scopeAmmoY = -0.30f; int scopeAmmoMask = 1, scopeAmmoKey = 12, scopeAmmoLight = 90, scopeAmmoOwn = 1, hidePrompts = 1, hideHurt = 1, ironSights = 0; float scopeTestMs = 500.0f;
    int   scopeInstant = 1; int scopeNeedsGrip = 1;   // scope view only with the left hand on the weapon   // scoped weapons: scope view opens/closes with the face distance, no raise animation
    int   scopeRound = 1; float scopeRoundSize = 0.8f, scopeHudX = 0.83f, scopeHudY = 0.925f;   // round scope window, ammo crop
    int   scopeFromWeapon = 1; float scopeSmoothing = 0.06f;   // scope view follows the weapon (s of smoothing)
    int   testCheats = 0; int   reticleDot = 1; float reticleDistance = 10.0f, reticleSize = 0.06f;   // aiming dot    // m: left hand this close to the foregrip + left grip = two-handed
    int   shadowMapSize = 2720;    // sun shadow cascade resolution (game: 2048 at Ultra High), max 2730, 0 = game's           // sun shadow cascades cover the whole VR view in cull mode 1   // hidden extra render area per side (fraction of the half-FOV) for screen-space reflections
    int   hangWatch = 1;            // freeze/crash reports (fc2vr_hang_N.txt/.dmp)
    float pauseMenuScale = 0.75f;  // in-game pause menu panel size factor (on top of HudScale)
    float hudScale = 0.8f;        // extra scale of the head-locked menu/prompt layer
    float lodScaleMul = 2.0f, killLodScaleMul = 3.0f, clustersLodScaleMul = 2.0f, realTreesLodScaleMul = 2.0f, detailDistMul = 2.0f, minSizeMul = 0.5f;
    float uiHeadY = 0.0f;         // metres: vertical offset of the head-locked UI layer
    int   cullMode = 1;           // see g_cullMode
    int   shareShadows = 1;       // render the shadow maps once per frame (left eye) and reuse them for the right eye
    int   uiScope = 2;            // 0 = only while the UI's own render target is bound, 1 = whole UI tail    // metres added to the view height (negative = lower)
    int   profile = 0;            // 1 = GPU timestamps + main-thread sampler (diagnostics; costs some CPU)
    int   skipRedundantReset = 1; // skip swap-chain resizes that change nothing (breaks a per-frame resize loop)
    int   recenterKey = VK_F8;
    int   toggleKey = VK_F9;
    int   hudSnap = 0;              // v228 test: HUD + eye snapshots when a UI element appears in gameplay (HudSnap)
    int   devKeys = 0;              // DevKeys: 1 = the developer test keys (F2 F3 F5 F6 F7 F10 F11 Pause) - off for release (v225)
} g_cfg;
static bool g_treeCache = false;
static volatile LONG g_treeCacheHits = 0;
static volatile LONG g_treeParked = 0, g_treeFreed = 0;
static double g_treeMakeMs = 0, g_rfMs = 0;
static volatile LONG g_resetSkips = 0, g_resetCalls = 0;
static void TreeParkAge();

static SHORT DevKey(int vk);
static float IniF(const char* k, float def) {
    char path[MAX_PATH]; snprintf(path, MAX_PATH, "%sfc2vr.ini", g_dir);
    char buf[64], d[64]; snprintf(d, 64, "%g", def);
    GetPrivateProfileStringA("VR", k, d, buf, 64, path);
    return (float)atof(buf);
}
static void LoadConfig() {
    g_cfg.enable     = (int)IniF("Enable", 1);
    g_cfg.worldScale = IniF("WorldScale", 1.0f);
    g_cfg.cullMargin = IniF("CullMargin", 1.25f);
    g_cfg.adaptiveDetail = 0;   /* v201: removed - it swung distant detail on and off around its target (pop-in at 150% + DLAA) and never saved frame time */ g_cfg.adaptiveTarget = IniF("AdaptiveDetailTargetMs", 10.0f); g_cfg.adaptiveMax = IniF("AdaptiveDetailMax", 1.0f);
    g_cfg.renderScale = IniF("RenderScale", 1.0f);
    g_cfg.steadyCam = (int)IniF("SteadyCamera", 1);
    g_cfg.monoForward = IniF("MonocularForward", 0.30f);
    g_cfg.flashForward = IniF("FlashlightForward", 0.10f);
    xr::SetMonoViewScale(IniF("MonocularViewScale", 1.15f));
    xr::SetMonoRingSize(IniF("MonocularRingSize", 0.90f));
    xr::SetHudTopBand(IniF("HudTopBand", 0.22f), IniF("HudTopDrop", 0.22f));
    g_cfg.skipRedundantReset = (int)IniF("SkipRedundantReset", 1);
    g_treeCache = IniF("TreeCache", 0) != 0;     // experimental; unnecessary once DpiAware stops the resize loop
    g_cfg.profile = (int)IniF("Profile", 0);
    g_cfg.devKeys = (int)IniF("DevKeys", 0);
    g_cfg.hudSnap = (int)IniF("HudSnap", 0);
    g_cfg.dpiAware = (int)IniF("DpiAware", 1);
    g_cfg.heightAdapt = IniF("HeightAdapt", 15.0f);
    g_cfg.heightOffset = IniF("HeightOffset", 0.0f);
    g_cfg.menuDistance = IniF("MenuDistance", 3.0f);
    g_cfg.fixNearPass = (int)IniF("FixNearPass", 1);
    g_cfg.hudLayer = (int)IniF("HudLayer", 1);
    g_cfg.hudDistance = IniF("HudDistance", 3.0f);
    g_cfg.hudFov = IniF("HudFov", 60.0f);
    g_cfg.menuWidth = IniF("MenuWidth", 2.4f);
    g_cfg.uiMode = (int)IniF("UiMode", 2);
    g_cfg.uiDepth = IniF("UiDepth", 6.0f);
    g_cfg.uiScale = IniF("UiScale", 0.7f);
    g_cfg.uiScope = (int)IniF("UiScope", 3);
    g_cfg.uiHeadY = IniF("UiHeadY", 0.0f);
    g_cfg.hudScale = IniF("HudScale", 0.8f);
    g_cfg.pauseMenuScale = IniF("PauseMenuScale", 0.75f);
    g_cfg.fixBouncingNpcs = (int)IniF("FixBouncingNpcs", 1);
    g_cfg.hangWatch = (int)IniF("HangWatch", 1);
    g_cfg.symmetricFov = (int)IniF("SymmetricFov", 1);
    g_cfg.shadowFix = (int)IniF("ShadowFix", 1);
    g_cfg.shadowRadial = IniF("ShadowRadialScale", 7.0f); if (g_cfg.shadowRadial < 0) g_cfg.shadowRadial = 0; if (g_cfg.shadowRadial > 10) g_cfg.shadowRadial = 10;
    g_cfg.shadowFarScale = IniF("ShadowFarScale", 3.0f); if (g_cfg.shadowFarScale < 1.0f) g_cfg.shadowFarScale = 1.0f; if (g_cfg.shadowFarScale > 5.0f) g_cfg.shadowFarScale = 5.0f;
    g_cfg.shadowMapSize = (int)IniF("ShadowMapSize", 2720); if (g_cfg.shadowMapSize > 2730) g_cfg.shadowMapSize = 2730; if (g_cfg.shadowMapSize < 0) g_cfg.shadowMapSize = 0; g_cfg.shadowMapSize &= ~15;
    g_cfg.overscanH = fmaxf(0.0f, fminf(1.0f, IniF("OverscanH", 0.15f)));
    g_cfg.overscanV = fmaxf(0.0f, fminf(1.0f, IniF("OverscanV", 0.25f)));
    g_cfg.devConsole = (int)IniF("DevConsole", 1);
    g_cfg.scopeMode = (int)IniF("ScopeMode", 1);
    g_cfg.scopeFovRatio = IniF("ScopeFovRatio", 0.6f);
    g_cfg.scopeDistance = IniF("ScopeDistance", 1.5f);
    g_cfg.scopeWidth = IniF("ScopeWidth", 1.2f);
    g_cfg.scopeFollowHead = (int)IniF("ScopeFollowHead", 0);
    g_cfg.npcFixThreads = (int)IniF("NpcFixThreads", 1); g_cfg.npcFixJobThreads = (int)IniF("NpcFixJobThreads", 1);
    g_cfg.lodScaleMul = IniF("LodScaleMul", 2.0f);
    { extern bool g_lodOn; g_lodOn = IniF("LodBoost", 1.0f) != 0.0f; }   // ini only (the F4 keys are gone)
    { extern float g_lodLevelIni; g_lodLevelIni = 1.0f;   /* v199: the game's own graphics menu decides LOD (LodLevel > 1 raised LodScale, which lowers detail) */ extern int g_noAntiPortals, g_noVisTest; g_noAntiPortals = (int)IniF("DisableAntiPortals", 1); g_noVisTest = (int)IniF("DisableVisibilityTest", 1); }
    g_cfg.killLodScaleMul = IniF("KillLodScaleMul", 3.0f);
    g_cfg.clustersLodScaleMul = IniF("ClustersLodScaleMul", 2.0f);
    g_cfg.realTreesLodScaleMul = IniF("RealTreesLodScaleMul", 2.0f);
    g_cfg.detailDistMul = IniF("DetailDistanceMul", 2.0f);
    g_cfg.minSizeMul = IniF("MinSizeMul", 0.5f);
    g_cfg.reticleTexMax = (int)IniF("ReticleTexMax", 256);
    { char path[MAX_PATH]; snprintf(path, MAX_PATH, "%sfc2vr.ini", g_dir); GetPrivateProfileStringA("VR", "UiSkipTex", "256x512x71v18", g_cfg.uiSkipTex, sizeof g_cfg.uiSkipTex, path); }
    g_cfg.shareShadows = (int)IniF("ShareShadows", 1);
    g_cfg.cullMode = (int)IniF("CullMode", 1); if (g_cfg.cullMode < 0 || g_cfg.cullMode > 3) g_cfg.cullMode = 1;
    if (g_cfg.uiMode) g_cfg.hudLayer = 0;
    LOG("config: enable=%d worldScale=%g cullMargin=%g renderScale=%g", g_cfg.enable, g_cfg.worldScale, g_cfg.cullMargin, g_cfg.renderScale);
    { dlssfc2::Config dc; dc.enable = (int)IniF("Dlss", 0); dc.preset = (int)IniF("DlssPreset", 11); dc.jitter = (int)IniF("DlssJitter", 1);
      dc.phases = (int)IniF("DlssJitterPhases", 16); dc.mask = (int)IniF("DlssMask", 1); dc.sharpness = IniF("DlssSharpness", 0.0f); dc.autoExposure = (int)IniF("DlssAutoExposure", 0);
      dc.quality = (int)IniF("DlssQuality", 0); if (dc.quality < 0 || dc.quality > 5) dc.quality = 0;
      dlssfc2::Configure(dc, g_dir); }
    { afw::Config ac; ac.enable = (int)IniF("Afw", 0); ac.gridStep = (int)IniF("AfwGridStep", 2); ac.handDepth = IniF("AfwHandDepth", 0.3f);
      ac.handMin = IniF("AfwHandMinDepth", 0.30f); ac.debug = (int)IniF("AfwDebug", 0); ac.hands = (int)IniF("AfwHands", 0); ac.handMotion = (int)IniF("AfwHandMotion", 2); afw::Configure(ac); }
}

// ============================================================ engine addresses (preferred base 0x10000000)
static uintptr_t g_base = 0;
static int g_capState = 0;       // F7 capture: 0 idle, 1 armed (next frame), 2 capturing this frame
static ID3D10Texture2D* g_eyeTex = nullptr;  // the D3D10 texture inside the engine eye colour target
static inline uintptr_t A(uintptr_t va) { uint32_t v = gameaddr::Xlate((uint32_t)va); return v ? g_base + (v - 0x10000000) : 0; }   // Steam VA -> loaded address (GOG translated)
static inline uint32_t V(uint32_t va) { return gameaddr::Xlate(va); }   // Steam VA -> this build's VA (for compares against reverse-mapped addresses)

namespace eng {
constexpr uintptr_t RenderViewport   = 0x1033F6F0; // __thiscall, 13 stack args, ret 34h
constexpr uintptr_t RenderViewportRA = 0x1034626A; // return address inside RenderFrame's viewport loop
constexpr uintptr_t PresentWrapper   = 0x10420D50; // CGraphicsD3D10 present, __thiscall(int), ret 4
constexpr uintptr_t UpdateMatrices   = 0x1040C800; // SceneCamera rebuild, this in ECX
constexpr uintptr_t BuildFrustum     = 0x104324B0; // cdecl(frustum = cam+0x22C, cam)
constexpr uintptr_t GraphicsDriver   = 0x11609668; // CGraphicsD3D10*
constexpr uintptr_t DriverVtableD3D10 = 0x10E52CA8;
constexpr uintptr_t CreateColorTarget = 0x1040D040; // cdecl(w,h,1,fmt,1,0,0,0,1,2) -> engine RT (as the screenshot path)
constexpr uintptr_t CreateDepthTarget = 0x1040D9F0; // cdecl(w,h,0x4B D24S8,1,0,0,0)
constexpr int EngineFmtLDR = 0x16;                   // colour format the screenshot path uses for LDR tiles
const uint8_t RenderViewportPrologue[5] = { 0x83, 0xEC, 0x24, 0x53, 0x56 };
const uint8_t PresentPrologue[5]        = { 0x53, 0x55, 0x56, 0x8B, 0xF1 };
}
namespace cam { // SceneCamera field offsets (0x3D0 bytes), verified live
constexpr int Aspect = 0x18, ProjType = 0x20, View = 0x30, World = 0xB0, Proj = 0xF0, InvProj = 0x130,
              ViewProj = 0x170, InvViewProj = 0x1B0, Pos = 0x1F0, Near = 0x220, Far = 0x224, Fov = 0x228,
              Frustum = 0x22C, InDir = 0x3B8, InUp = 0x3C4;
}
namespace vp { constexpr int CullCam = 0x20, RenderCam = 0x3F0; }

typedef void (__fastcall *UpdateMatrices_t)(void* cam);
typedef void (__cdecl *BuildFrustum_t)(void* frustum, void* cam);
typedef void* (__cdecl *CreateColorTarget_t)(int, int, int, int, int, int, int, int, int, int);
typedef void* (__cdecl *CreateDepthTarget_t)(int, int, int, int, int, int, int);

// ============================================================ inline patching
static void* MakeTrampoline(uintptr_t target, const uint8_t* prologue, size_t n) {
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, prologue, n);
    t[n] = 0xE9; *(int32_t*)(t + n + 1) = (int32_t)((target + n) - ((uintptr_t)t + n + 5));
    FlushInstructionCache(GetCurrentProcess(), t, 64);
    return t;
}
static bool PatchJmpN(uintptr_t target, const uint8_t* expect, size_t n, void* hook, void** tramp) {
    if (!target) { LOG("patch: no address on this game version, not applied"); return false; }
    if (memcmp((void*)target, expect, n) != 0) {
        const uint8_t* b = (const uint8_t*)target;
        LOG("patch %08x: unexpected bytes %02x %02x %02x %02x %02x", (unsigned)target, b[0], b[1], b[2], b[3], b[4]);
        return false;
    }
    *tramp = MakeTrampoline(target, expect, n);
    DWORD old; VirtualProtect((void*)target, n, PAGE_EXECUTE_READWRITE, &old);
    uint8_t j[16]; memset(j, 0x90, sizeof(j));
    j[0] = 0xE9; *(int32_t*)(j + 1) = (int32_t)((uintptr_t)hook - (target + 5));
    memcpy((void*)target, j, n);
    VirtualProtect((void*)target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, n);
    return true;
}
static bool PatchJmp(uintptr_t target, const uint8_t* expect, void* hook, void** tramp) {
    return PatchJmpN(target, expect, 5, hook, tramp);
}

// ============================================================ math (row-vector convention, as the engine)
using V3 = fc2vr::tracking::Vec3;
static inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Norm(V3 a) { float l = sqrtf(Dot(a, a)); return l > 1e-8f ? a * (1.0f / l) : a; }
static inline V3 QRot(const XrQuaternionf& q, V3 v) { // v' = q v q*
    V3 u{ q.x, q.y, q.z }; float s = q.w;
    V3 c1{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    V3 c2{ u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x };
    return v + c1 * (2.0f * s) + c2 * 2.0f;
}
static inline V3 ToV3(const XrVector3f& v) { return { v.x, v.y, v.z }; }

static void MatMul(const float* a, const float* b, float* o) { // o = a*b (4x4 row-major)
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
        r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, sizeof(r));
}
static bool MatInv(const float* m, float* o) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
    return true;
}

// ============================================================ camera access
template <class T> static inline T& F(uint8_t* p, int off) { return *(T*)(p + off); }
static inline V3 GetV3(uint8_t* c, int off) { return { F<float>(c, off), F<float>(c, off + 4), F<float>(c, off + 8) }; }
static inline void SetV3(uint8_t* c, int off, V3 v) { F<float>(c, off) = v.x; F<float>(c, off + 4) = v.y; F<float>(c, off + 8) = v.z; }

struct CamSave { V3 pos, dir, up; float fov, aspect; };
static CamSave SaveCam(uint8_t* c) {
    return { GetV3(c, cam::Pos), GetV3(c, cam::InDir), GetV3(c, cam::InUp), F<float>(c, cam::Fov), F<float>(c, cam::Aspect) };
}
static void Rebuild(uint8_t* c) { ((UpdateMatrices_t)A(eng::UpdateMatrices))(c); }
static void RestoreCam(uint8_t* c, const CamSave& s) {
    SetV3(c, cam::Pos, s.pos); SetV3(c, cam::InDir, s.dir); SetV3(c, cam::InUp, s.up);
    F<float>(c, cam::Fov) = s.fov; F<float>(c, cam::Aspect) = s.aspect;
    Rebuild(c);
}

// Place a SceneCamera at pos looking along dir/up with the given (possibly asymmetric) tangent extents.
// tl/tr/td/tu are tan(angle) as OpenXR reports them (tl, td negative).
// The eye camera currently posed with an off-axis projection (read by the near-pass projection hook).
static uint8_t* g_eyeCam = nullptr;
static float g_eyeTan[4];   // left, right, down, up
static void PoseCamera(uint8_t* c, V3 pos, V3 dir, V3 up, float tl, float tr, float td, float tu, bool exactProj) {
    // Symmetric bounding values for consumers that read scalar fov/aspect (LOD, near-plane size).
    float th = fmaxf(fabsf(tl), fabsf(tr)), tv = fmaxf(fabsf(td), fabsf(tu));
    SetV3(c, cam::Pos, pos); SetV3(c, cam::InDir, dir); SetV3(c, cam::InUp, up);
    F<float>(c, cam::Fov) = 2.0f * atanf(th);   // FOV mode 0: horizontal
    F<float>(c, cam::Aspect) = th / tv;
    Rebuild(c);                                   // View, World, symmetric Proj, inverses, frustum
    if (!exactProj) return;
    g_eyeCam = c; g_eyeTan[0] = tl; g_eyeTan[1] = tr; g_eyeTan[2] = td; g_eyeTan[3] = tu;
    // Exact off-axis projection, same convention as the engine's builder (RH, [11] = -1, standard depth).
    float n = F<float>(c, cam::Near), f = F<float>(c, cam::Far);
    float* P = &F<float>(c, cam::Proj);
    memset(P, 0, 64);
    P[0] = 2.0f / (tr - tl);
    P[5] = 2.0f / (tu - td);
    P[8] = (tr + tl) / (tr - tl);
    P[9] = (tu + td) / (tu - td);
    P[10] = f / (n - f);
    P[11] = -1.0f;
    P[14] = n * f / (n - f);
    MatInv(P, &F<float>(c, cam::InvProj));
    MatMul(&F<float>(c, cam::View), P, &F<float>(c, cam::ViewProj));
    MatInv(&F<float>(c, cam::ViewProj), &F<float>(c, cam::InvViewProj));
    ((BuildFrustum_t)A(eng::BuildFrustum))(c + cam::Frustum, c);
}

// ============================================================ head tracking -> game space
static double g_refYTime = 0;
static int g_rsDelay = 1; static bool g_rsOn = false;
static int g_motionTrace = 0;
static int g_motionDiag = 0; static int g_camBodyClock = 1; static long g_camBodyUsed = 0;   // v231 CameraOnBodyClock   // v231: frames left to trace camera / bone / player positions (F4)   // v231 room-scale (see arms RoomScaleUpdate)
struct Recenter { bool valid = false; V3 pos{}; V3 fwdH{ 0, 0, -1 }; V3 rightH{ 1, 0, 0 }; } g_ref;

static void DoRecenter(const XrPosef& head) {
    if (!fc2vr::authority::FinitePosition(head) || !fc2vr::authority::FiniteOrientation(head)) return;
    V3 f = QRot(head.orientation, { 0, 0, -1 });
    V3 fh = Norm({ f.x, 0, f.z });
    g_ref.pos = ToV3(head.position);
    g_ref.fwdH = fh;
    g_ref.rightH = { -fh.z, 0, fh.x };
    g_ref.valid = true;
    xr::InvalidateForRecenter();
    g_refYTime = 0;   // set by caller via NowMs when available
    LOG("recentered (head y=%.3f)", head.position.y);
}
// Height: the game's camera already sits at the character's eye height, so only deviations from the player's
// *sustained* real head height should move it. The vertical reference follows the head slowly
// (HeightAdapt seconds); it snaps for the first second of tracking and on F8.
static void UpdateHeightRef(float headY, double nowMs) {
    if (!g_ref.valid) return;
    static double last = 0, startMs = 0;
    if (startMs == 0) startMs = nowMs;
    double dt = last > 0 ? (nowMs - last) * 0.001 : 0; last = nowMs;
    if (nowMs - startMs < 1000.0 || nowMs - g_refYTime < 1000.0) { g_ref.pos.y = headY; return; }
    if (g_cfg.heightAdapt > 0 && dt > 0 && dt < 0.5) {
        float k = (float)(1.0 - exp(-dt / g_cfg.heightAdapt));
        g_ref.pos.y += (headY - g_ref.pos.y) * k;
    }
}
static inline fc2vr::tracking::ReferenceFrame TrackingReference() {
    return { g_ref.pos, g_ref.rightH, g_ref.fwdH, g_cfg.heightOffset };
}
using GameBasis = fc2vr::tracking::GameBasis;
using PoseFrame = fc2vr::tracking::PoseFrame;
// Yaw-only basis from the game's camera: the mouse still turns the body; the headset adds look.
static GameBasis BasisFromGameCam(uint8_t* c) {
    const float* W = &F<float>(c, cam::World);         // rows: right, forward, up, position
    V3 fwd{ W[4], W[5], W[6] };
    V3 Fh = Norm({ fwd.x, fwd.y, 0 });
    if (fabsf(Fh.x) + fabsf(Fh.y) < 1e-4f) Fh = Norm({ W[0], W[1], 0 }), Fh = { -Fh.y, Fh.x, 0 };
    GameBasis b;
    b.F = Fh; b.U = { 0, 0, 1 }; b.R = { Fh.y, -Fh.x, 0 };  // R = F x U
    b.P = GetV3(c, cam::Pos);
    return b;
}
// Existing SteadyCamera policy: smooth native lateral/vertical camera bob and
// sprint yaw sway at stick rest; follow forward motion and deliberate turns.
// Bypass in vehicles, mounted guns and special controls. Prepare once, then share.
static inline double NowMs();
static bool g_riding = false;
static PoseFrame PreparePoseFrame(uint32_t sequence, uint8_t* rc, bool stabilize) {
    static fc2vr::tracking::PoseFrameBuilder builder;
    fc2vr::tracking::SteadyInput input;
    input.nowMs = NowMs();
    float pos[3]{}, heading = 0;
    input.eligible = stabilize && g_cfg.steadyCam && controls::NormalControls() && controls::VehicleMode() == 0
        && !controls::MountedWeapon() && arms::PlayerState(pos, &heading);
    input.playerPosition = {pos[0],pos[1],pos[2]};
    input.turning = fabsf(xr::Hand(1).stick.x) > 0.15f;
    {   // v232: riding (the intro taxi, a bus, any vehicle the controls don't report): the player moves without the stick.
        // SteadyCamera would smooth the view while the body and the car interior move with the vehicle - they judder
        // in the view. Not eligible while riding.
        static float lp[3]; static double lt = 0, still = 0; static bool riding = false;
        double tn = input.nowMs; float dt = lt > 0 ? (float)((tn - lt) * 0.001) : 0; lt = tn;
        float dx = pos[0] - lp[0], dy = pos[1] - lp[1]; lp[0] = pos[0]; lp[1] = pos[1]; lp[2] = pos[2];
        float spd = dt > 0.0005f && dt < 0.1f ? sqrtf(dx * dx + dy * dy) / dt : 0;
        const xr::HandState& L = xr::Hand(0); bool stick = fabsf(L.stick.x) > 0.2f || fabsf(L.stick.y) > 0.2f;
        if (!stick && spd > 2.5f && spd < 60.0f) still += dt * 1000.0; else if (stick || spd < 1.0f) still = 0;
        bool r = still > 400.0;
        if (r != riding) { riding = r; LOG("steady camera: %s", r ? "riding (off)" : "on foot"); }
        g_riding = riding;
        if (riding) input.eligible = false;
    }
    GameBasis gb = BasisFromGameCam(rc);
    {   // v231 camera on the body clock: the game's camera is one game frame behind the player's body (it moves by the
        // body's previous step), so the body, arms and weapon jitter by the frame-to-frame step difference. The body's
        // Camera bone from the latest skeleton update is where the camera will be next frame: use it.
        static long lastSeq = 0; long seq = arms::CamBoneSeq();
        float bp[3];
        if (g_camBodyClock && seq != lastSeq && arms::CamBonePos(bp)) {
            float dx = bp[0] - gb.P.x, dy = bp[1] - gb.P.y, dz = bp[2] - gb.P.z;
            if (dx * dx + dy * dy + dz * dz < 0.6f * 0.6f) { gb.P = { bp[0], bp[1], bp[2] }; ++g_camBodyUsed; }
        }
        lastSeq = seq;
    }
    PoseFrame frame = builder.Build(sequence, gb, TrackingReference(), g_cfg.worldScale, input);
    {   // v232: when SteadyCamera stops applying (riding, a vehicle), its offset fades out instead of jumping
        static V3 carry{ 0, 0, 0 }, lastOff{ 0, 0, 0 }; static bool wasSmoothed = false; static double lt = 0; static uint64_t lastSeq = ~0ull;
        if (sequence != lastSeq) {
            lastSeq = sequence; double tn = input.nowMs; float dt = lt > 0 ? (float)((tn - lt) * 0.001) : 1; lt = tn;
            V3 off = frame.game.P - frame.raw.P;
            bool smoothed = input.eligible;
            if (wasSmoothed && !smoothed && Dot(lastOff, lastOff) < 0.25f) carry = lastOff;
            wasSmoothed = smoothed; lastOff = off;
            if (!smoothed) { carry = carry * (dt < 1 ? expf(-dt / 0.3f) : 0.0f); frame.game.P = frame.raw.P + carry; }
            else carry = { 0, 0, 0 };
        } else if (!input.eligible) frame.game.P = frame.raw.P + carry;
    }
    if (sequence % 900 == 0) LOG("pose frame shared-camera-1 seq=%llu steadyEligible=%d rawP=%.3f,%.3f,%.3f effectiveP=%.3f,%.3f,%.3f rawF=%.4f,%.4f effectiveF=%.4f,%.4f",
        (unsigned long long)frame.sequence, input.eligible, frame.raw.P.x, frame.raw.P.y, frame.raw.P.z,
        frame.game.P.x, frame.game.P.y, frame.game.P.z, frame.raw.F.x, frame.raw.F.y, frame.game.F.x, frame.game.F.y);
    return frame;
}
static void GripsGame(const PoseFrame& frame, float out[2][6]) {   // the controllers' grips and a point 0.4 m ahead of each, game space (AFW hands)
    for (int i = 0; i < 2; ++i) {
        const xr::HandState& h = xr::Hand(i);
        for (int k = 0; k < 6; ++k) out[i][k] = NAN;
        if (!h.gripValid) continue;
        V3 p = frame.WorldPosition(ToV3(h.grip.position));
        out[i][0] = p.x; out[i][1] = p.y; out[i][2] = p.z;
        if (!h.aimValid) continue;
        V3 tp = ToV3(h.grip.position) + QRot(h.aim.orientation, { 0, 0, -1 }) * 0.4f;
        V3 t = frame.WorldPosition(tp);
        out[i][3] = t.x; out[i][4] = t.y; out[i][5] = t.z;
    }
}


static void ModOff(uintptr_t a, char* out, size_t n);
// ============================================================ profiling
static double g_qpcToMs = 0;
static inline double NowMs() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * g_qpcToMs; }
static bool g_treeSuppress = false;
static volatile LONG g_treeMakes = 0, g_treeMakesEye = 0;
static uint32_t g_frame = 0;
static bool g_inEye = false;
static ID3D10Device* g_dev10 = nullptr;
static int g_renderEye = -1;   // eye whose scene is being rendered (-1 = none)
static int g_eyeSize[2] = { 0, 0 };
static int g_outSize[2] = { 0, 0 };   // DLSS upscaling: the shown eye size (the eyes render g_eyeSize); 0 = same size
static bool g_upscale = false;
static volatile float g_nearMaxDepth = 0.0f;       // largest depth the near pass (arms, weapon) writes (DLSS mask)
static volatile LONG g_treeCalls = 0, g_treeRebuilds = 0, g_treeSkipped = 0;
static volatile LONG g_nearProjEye = 0, g_uiBegins = 0;
struct Prof { double wait = 0, eye0 = 0, eye1 = 0, cams = 0, copies = 0, end = 0, present = 0, frame = 0; int n = 0; } g_prof;
static double g_lastFrameStart = 0;
static volatile LONG g_texCreates = 0, g_rtvCreates = 0;
static LONG g_texLogged = 0;
static LONG g_rtvLogged = 0;

typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D10Device*, const D3D10_TEXTURE2D_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Texture2D**);
typedef HRESULT (STDMETHODCALLTYPE *CreateRTV_t)(ID3D10Device*, ID3D10Resource*, const D3D10_RENDER_TARGET_VIEW_DESC*, ID3D10RenderTargetView**);
static bool g_profArmed = false;   // log resource creation only once VR frames are running
static CreateTexture2D_t o_CreateTexture2D = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *CreateBuffer_t)(ID3D10Device*, const D3D10_BUFFER_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Buffer**);
static CreateBuffer_t o_CreateBuffer = nullptr;
static volatile LONG g_bufCreates = 0; static LONG g_bufLogged = 0; static LONGLONG g_bufBytes = 0;
static void ModOff(uintptr_t a, char* out, size_t n);
struct BufSite { uint32_t c[4]; int usage, bind; uint32_t n; uint64_t bytes; };
static BufSite g_bufSites[128];
static void* g_capRTV0 = nullptr; static int g_capEye = 0;
#include <string>
#include <vector>
static void ShowToast(const char* t);
namespace cbcap { void NoteBuffer(ID3D10Buffer* b); void HookCreateVS(void** vt); void Install(void** vt); void Arm(); void Present(const char* dir); void NoteBones(const float* W, const float* models, int n); }
static HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D10Device* d, const D3D10_BUFFER_DESC* bd, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out) {
    InterlockedIncrement(&g_bufCreates);
    if (bd) g_bufBytes += bd->ByteWidth;
    if (g_profArmed && bd) {
        uint32_t c[4] = { 0, 0, 0, 0 }; int found = 0;
        uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
        for (int i = 0; i < 600 && found < 4; ++i) { uintptr_t v = sp[i]; if (g_base && v > g_base + 0x1000 && v < g_base + 0xE00000) c[found++] = (uint32_t)(v - g_base + 0x10000000); }
        for (auto& e : g_bufSites) {
            if (e.n && memcmp(e.c, c, sizeof c) == 0 && e.usage == (int)bd->Usage && e.bind == (int)bd->BindFlags) { ++e.n; e.bytes += bd->ByteWidth; break; }
            if (!e.n) { memcpy(e.c, c, sizeof c); e.usage = bd->Usage; e.bind = bd->BindFlags; e.n = 1; e.bytes = bd->ByteWidth; break; }
        }
    }
    HRESULT hr = o_CreateBuffer(d, bd, init, out);
    if (SUCCEEDED(hr) && bd && (bd->BindFlags & D3D10_BIND_CONSTANT_BUFFER) && out && *out) cbcap::NoteBuffer(*out);
    return hr;
}
static CreateRTV_t o_CreateRTV = nullptr;
static HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D10Device* d, const D3D10_TEXTURE2D_DESC* td, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out) {
    InterlockedIncrement(&g_texCreates);
    if (g_profArmed && td && g_texLogged < 40) { ++g_texLogged; LogWrite("  CreateTexture2D %ux%u fmt=%d bind=%x misc=%x", td->Width, td->Height, (int)td->Format, td->BindFlags, td->MiscFlags); }
    return o_CreateTexture2D(d, td, init, out);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateRTV(ID3D10Device* d, ID3D10Resource* r, const D3D10_RENDER_TARGET_VIEW_DESC* desc, ID3D10RenderTargetView** out) {
    InterlockedIncrement(&g_rtvCreates);
    if (g_profArmed && g_rtvLogged < 30) {
        ++g_rtvLogged;
        ID3D10Texture2D* tx = nullptr; D3D10_TEXTURE2D_DESC td{};
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&tx))) { tx->GetDesc(&td); tx->Release(); }
        char c1[96] = "", c2[96] = "", c3[96] = "";
        uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
        int found = 0; char* outs[3] = { c1, c2, c3 };
        for (int i = 0; i < 256 && found < 3; ++i) { uintptr_t v = sp[i]; if (g_base && v > g_base + 0x1000 && v < g_base + 0xE00000) ModOff(v, outs[found++], 96); }
        LogWrite("  CreateRTV on %ux%u fmt=%d  callers: %s %s %s", td.Width, td.Height, (int)td.Format, c1, c2, c3);
    }
    return o_CreateRTV(d, r, desc, out);
}

// ---- capture-frame draw tracing (eye 0 only): run-length log of render-target switches and draw callers
typedef void (STDMETHODCALLTYPE *DrawIndexed_t)(ID3D10Device*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE *Draw_t)(ID3D10Device*, UINT, UINT);
typedef void (STDMETHODCALLTYPE *OMSetRT_t)(ID3D10Device*, UINT, ID3D10RenderTargetView* const*, ID3D10DepthStencilView*);
static DrawIndexed_t o_DrawIndexed = nullptr; static Draw_t o_Draw = nullptr; static OMSetRT_t o_OMSetRT = nullptr;
static bool g_traceDraws = false;
static uint32_t g_runKey = 0, g_runCount = 0, g_runC[3]; static uintptr_t g_curRT = 0; static int g_drawSeq = 0;
static void FlushRun() {
    if (g_runCount) LogWrite("  draws x%-4u rt=%08x  %08x <- %08x <- %08x", g_runCount, (unsigned)g_curRT, g_runC[0], g_runC[1], g_runC[2]);
    g_runCount = 0; g_runKey = 0;
}
static ID3D10Device* g_vtDev = nullptr;
static void* g_seenSrv[256]; static int g_nSeenSrv = 0;
static void NoteDraw(void* frame, ID3D10Device* dev) {
    if (!g_traceDraws) return;
    uint32_t c[3] = { 0, 0, 0 }; int found = 0;
    uintptr_t* sp = (uintptr_t*)frame;
    for (int i = 0; i < 300 && found < 3; ++i) { uintptr_t v = sp[i]; if (g_base && v > g_base + 0x1000 && v < g_base + 0xE00000) c[found++] = (uint32_t)(v - g_base + 0x10000000); }
    uint32_t key = c[0] * 31 + c[1] * 7 + c[2] + (uint32_t)g_curRT;
    if (key != g_runKey) { FlushRun(); g_runKey = key; memcpy(g_runC, c, sizeof c); }
    ++g_runCount; ++g_drawSeq;
    // Water capture: every texture a pixel shader samples is logged the first time it is seen this frame, so render-to-
    // texture results (reflection maps etc.) can be matched with the render targets saved by the capture.
    if (dev) {
        ID3D10ShaderResourceView* srv[16] = {}; dev->PSGetShaderResources(0, 16, srv);
        for (int k = 0; k < 16; ++k) if (srv[k]) {
            ID3D10Resource* r = nullptr; srv[k]->GetResource(&r);
            bool seen = false; for (int i = 0; i < g_nSeenSrv; ++i) if (g_seenSrv[i] == (void*)r) { seen = true; break; }
            if (!seen && r && g_nSeenSrv < 256) {
                g_seenSrv[g_nSeenSrv++] = r;
                ID3D10Texture2D* t = nullptr; char d[64] = "not 2D";
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) { D3D10_TEXTURE2D_DESC td; t->GetDesc(&td); snprintf(d, sizeof d, "%ux%u fmt=%d mips=%u bind=%x", td.Width, td.Height, (int)td.Format, td.MipLevels, td.BindFlags); t->Release(); }
                FlushRun();
                LogWrite("   PS t%d = tex %p %s  (draw %d, rt %08x, %08x <- %08x <- %08x)", k, (void*)r, d, g_drawSeq, (unsigned)g_curRT, c[0], c[1], c[2]);
            }
            if (r) r->Release(); srv[k]->Release();
        }
    }
}
namespace cbcap { void OnDraw(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t f, void* rtv, int eye); }
static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D10Device* d, UINT n, UINT s0, INT b) { cbcap::OnDraw(0x101, n, s0, (uint32_t)b, 0, 0, g_capRTV0, g_inEye ? g_capEye : 3); o_DrawIndexed(d, n, s0, b); }
static void STDMETHODCALLTYPE Hook_Draw(ID3D10Device* d, UINT n, UINT s0) { cbcap::OnDraw(0x102, n, s0, 0, 0, 0, g_capRTV0, g_inEye ? g_capEye : 3); o_Draw(d, n, s0); }
typedef void (STDMETHODCALLTYPE *DrawIdxInst_t)(ID3D10Device*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *DrawInst_t)(ID3D10Device*, UINT, UINT, UINT, UINT);
static DrawIdxInst_t o_DrawIdxInst = nullptr; static DrawInst_t o_DrawInst = nullptr;
static void STDMETHODCALLTYPE Hook_DrawIdxInst(ID3D10Device* d, UINT a, UINT b, UINT c, INT e, UINT f) { cbcap::OnDraw(0x103, a, b, c, (uint32_t)e, f, g_capRTV0, g_inEye ? g_capEye : 3); o_DrawIdxInst(d, a, b, c, e, f); }
static void STDMETHODCALLTYPE Hook_DrawInst(ID3D10Device* d, UINT a, UINT b, UINT c, UINT e) { o_DrawInst(d, a, b, c, e); }
// Inline hooks on the runtime implementations (catch every caller, cached pointers included).
static DrawIndexed_t t_DrawIndexed = nullptr; static Draw_t t_Draw = nullptr; static DrawIdxInst_t t_DrawIdxInst = nullptr; static DrawInst_t t_DrawInst = nullptr; static OMSetRT_t t_OMSetRT = nullptr;
namespace cbcap { void OnDraw(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t f, void* rtv, int eye); }
static void STDMETHODCALLTYPE I_DrawIndexed(ID3D10Device* d, UINT n, UINT s0, INT b) { NoteDraw(__builtin_frame_address(0), d); cbcap::OnDraw(1, n, s0, (uint32_t)b, 0, 0, g_capRTV0, g_inEye ? g_capEye : 3); t_DrawIndexed(d, n, s0, b); }
static void STDMETHODCALLTYPE I_Draw(ID3D10Device* d, UINT n, UINT s0) { NoteDraw(__builtin_frame_address(0), d); cbcap::OnDraw(2, n, s0, 0, 0, 0, g_capRTV0, g_inEye ? g_capEye : 3); t_Draw(d, n, s0); }
static void STDMETHODCALLTYPE I_DrawIdxInst(ID3D10Device* d, UINT a, UINT b, UINT c, INT e, UINT f) { NoteDraw(__builtin_frame_address(0), d); cbcap::OnDraw(3, a, b, c, (uint32_t)e, f, g_capRTV0, g_inEye ? g_capEye : 3); t_DrawIdxInst(d, a, b, c, e, f); }
static void STDMETHODCALLTYPE I_DrawInst(ID3D10Device* d, UINT a, UINT b, UINT c, UINT e) { NoteDraw(__builtin_frame_address(0), d); cbcap::OnDraw(4, a, b, c, e, 0, g_capRTV0, g_inEye ? g_capEye : 3); t_DrawInst(d, a, b, c, e); }
// ---- per-eye UI viewport (UiMode 1)
typedef void (STDMETHODCALLTYPE *RSSetVP_t)(ID3D10Device*, UINT, const D3D10_VIEWPORT*);
static RSSetVP_t o_RSSetVP = nullptr, t_RSSetVP = nullptr;
static bool g_uiShift = false;            // inside the UI tail of an eye pass
static bool g_uiOnEye = false, g_uiVpShifted = false;
// UiMode 2: the UI is drawn (eye 0 only) into a transparent eye-sized texture shown as a head-locked quad layer.
static ID3D10RenderTargetView* g_uiLayerRTV = nullptr;
static ID3D10RenderTargetView* g_uiSavedEyeRTV = nullptr; static ID3D10DepthStencilView* g_uiSavedEyeDSV = nullptr;
static void* g_scopeBBres = nullptr;   // scope view: screen-sized targets are treated as the eye target so the UI goes to the HUD layer
static UINT g_scrW = 0, g_scrH = 0;          // backbuffer size the screen-quad swapchain was made for
static bool ScopeUiTarget(ID3D10Resource* r) {   // during the scope view: any screen-sized render target
    if (!g_scopeBBres || !r) return false;
    if ((void*)r == g_scopeBBres) return true;
    ID3D10Texture2D* t = nullptr; bool ok = false;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) { D3D10_TEXTURE2D_DESC d; t->GetDesc(&d); ok = d.Width == g_scrW && d.Height == g_scrH; t->Release(); }
    return ok;
}
static bool g_uiRedirect = false, g_uiLayerFresh = false; static int g_uiCurEye = -1, g_uiDrawOrd = 0;
static int g_uiPrimary = 0;   // the eye whose pass draws the UI layer this frame (0; under AFW whichever eye is rendered)
static ID3D10RenderTargetView* g_uiRetRTV = nullptr; static ID3D10RenderTargetView* g_uiBoundLayer = nullptr; static bool g_uiRetFresh = false;
// Additive UI draws (dest blend ONE) would write opaque alpha into the layer; they get a copy of their blend state
// without alpha writes.
struct BlendFix { ID3D10BlendState* orig; ID3D10BlendState* fixed; };
static BlendFix g_blendFix[32]; static int g_nBlendFix = 0; static ID3D10BlendState* g_blendSetFixed = nullptr; static ID3D10BlendState* g_blendSetOrig = nullptr;
static bool g_uiShiftRT = false;          // the UI's render target is bound (scope 0)
static uintptr_t g_uiShiftRTV = 0;
static float g_uiCx = 0.5f, g_uiCy = 0.5f; // UI centre in the eye image, normalised (0..1, y down)
static D3D10_VIEWPORT g_uiVpOrig{}; static bool g_uiVpValid = false;
static V3 g_uiAim{}; static bool g_uiAimValid = false;   // aim point in game space for this frame
static D3D10_VIEWPORT UiXform(const D3D10_VIEWPORT& v) {
    D3D10_VIEWPORT o = v;
    float s = g_cfg.uiScale;
    float w = v.Width * s, h = v.Height * s;
    float cx = v.TopLeftX + g_uiCx * v.Width, cy = v.TopLeftY + g_uiCy * v.Height;
    o.TopLeftX = (INT)lroundf(cx - 0.5f * w); o.TopLeftY = (INT)lroundf(cy - 0.5f * h);
    o.Width = (UINT)lroundf(w); o.Height = (UINT)lroundf(h);
    return o;
}
static inline bool UiShiftNow() { return g_uiShift && (g_cfg.uiScope == 1 || g_uiShiftRT); }
static void ScanDrawSlots();
static void STDMETHODCALLTYPE I_RSSetVP(ID3D10Device* d, UINT n, const D3D10_VIEWPORT* vp) {
    ScanDrawSlots();
    // the near pass (arms, weapon) draws into a compressed depth range: its viewport's MaxDepth is where it ends (DLSS mask)
    if (g_inEye && n && vp && vp[0].MaxDepth < 0.999f && vp[0].MaxDepth > 0.0f && (int)vp[0].Width == g_eyeSize[0]) {
        if (vp[0].MaxDepth != g_nearMaxDepth) { static int logged = 0; if (logged++ < 4) LOG("eye viewport with depth range %.5f..%.5f (near pass)", vp[0].MinDepth, vp[0].MaxDepth); }
        g_nearMaxDepth = vp[0].MaxDepth;
    }
    if (g_traceDraws && n && vp) LogWrite(" RSSetViewports %d,%d %ux%u%s", vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height, g_uiShift ? " [UI]" : "");
    if (g_uiShift && n >= 1 && vp) {
        g_uiVpOrig = vp[0]; g_uiVpValid = true;
        if (g_cfg.uiScope == 3) { g_uiVpShifted = false; t_RSSetVP(d, n, vp); return; }
        if (UiShiftNow()) { D3D10_VIEWPORT t[16]; UINT k = n > 16 ? 16 : n; for (UINT i = 0; i < k; ++i) t[i] = UiXform(vp[i]); t_RSSetVP(d, k, t); return; }
    }
    t_RSSetVP(d, n, vp);
}
static void TraceMark(const char* m) { if (g_traceDraws) { FlushRun(); LogWrite(" ---- %s (draws so far %d)", m, g_drawSeq); } }
static void STDMETHODCALLTYPE Hook_OMSetRT(ID3D10Device* d, UINT n, ID3D10RenderTargetView* const* rtv, ID3D10DepthStencilView* dsv) { o_OMSetRT(d, n, rtv, dsv); }
static ID3D10Texture2D* g_capRTs[64]; static int g_nCapRTs = 0; static bool g_capCollect = false;
// DLSS: which depth buffers (and eye-size colour targets) the eye render really binds, with how often
struct BindCount { ID3D10Texture2D* t; int n; bool depth; };
static BindCount g_eyeBinds[24]; static int g_nEyeBinds = 0; static bool g_eyeBindsOn = false;
static void NoteEyeBind(ID3D10View* v, bool depth) {
    if (!v) return;
    ID3D10Resource* r = nullptr; v->GetResource(&r); if (!r) return;
    ID3D10Texture2D* t = nullptr; if (FAILED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t)) || !t) { r->Release(); return; }
    r->Release();
    D3D10_TEXTURE2D_DESC td; t->GetDesc(&td);
    if ((int)td.Width == g_eyeSize[0] && (int)td.Height == g_eyeSize[1]) {
        for (int i = 0; i < g_nEyeBinds; ++i) if (g_eyeBinds[i].t == t) { ++g_eyeBinds[i].n; t->Release(); return; }
        if (g_nEyeBinds < 24) { g_eyeBinds[g_nEyeBinds++] = { t, 1, depth }; return; }   // keeps the reference
    }
    t->Release();
}
static void STDMETHODCALLTYPE I_OMSetRT(ID3D10Device* d, UINT n, ID3D10RenderTargetView* const* rtv, ID3D10DepthStencilView* dsv) {
    g_capRTV0 = (rtv && n) ? (void*)rtv[0] : nullptr;
    ScanDrawSlots();
    if (g_eyeBindsOn && g_inEye) { NoteEyeBind(dsv, true); if (rtv) for (UINT k = 0; k < n && k < 8; ++k) NoteEyeBind(rtv[k], false); }
    if (g_capCollect && rtv) for (UINT k = 0; k < n && k < 8; ++k) if (rtv[k] && g_nCapRTs < 64) {
        ID3D10Resource* r = nullptr; rtv[k]->GetResource(&r); ID3D10Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) {
            bool have = false; for (int i = 0; i < g_nCapRTs; ++i) if (g_capRTs[i] == t) have = true;
            D3D10_TEXTURE2D_DESC td; t->GetDesc(&td);
            if (!have && td.SampleDesc.Count == 1) g_capRTs[g_nCapRTs++] = t; else t->Release();
        }
        if (r) r->Release();
    }
    if (g_traceDraws) {
        uintptr_t rt = (n && rtv && rtv[0]) ? (uintptr_t)rtv[0] : 0;
        if (rt != g_curRT) { FlushRun(); g_curRT = rt;
            char desc[64] = "";
            if (rt) { ID3D10Resource* r = nullptr; rtv[0]->GetResource(&r); ID3D10Texture2D* t = nullptr;
                if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) { D3D10_TEXTURE2D_DESC td; t->GetDesc(&td); snprintf(desc, 64, "tex %p %ux%u fmt=%d%s", (void*)r, td.Width, td.Height, (int)td.Format, t == g_eyeTex ? " [EYE]" : ""); t->Release(); }
                if (r) r->Release(); }
            LogWrite(" OMSetRenderTargets rt=%08x %s", (unsigned)rt, desc); }
    }
    if (g_uiShift && g_cfg.uiMode == 2) {
        bool on = false;
        if (n && rtv && rtv[0]) { ID3D10Resource* r = nullptr; rtv[0]->GetResource(&r); on = r && ((void*)r == (void*)g_eyeTex || ScopeUiTarget(r)); if (r) r->Release(); }
        g_uiOnEye = on; g_uiRedirect = false;
    } else if (g_uiShift && g_cfg.uiScope == 3) {
        bool on = false;
        if (n && rtv && rtv[0]) { ID3D10Resource* r = nullptr; rtv[0]->GetResource(&r); on = r && (void*)r == (void*)g_eyeTex; if (r) r->Release(); }
        g_uiOnEye = on;
        if (!on && g_uiVpShifted && g_uiVpValid) { t_OMSetRT(d, n, rtv, dsv); t_RSSetVP(d, 1, &g_uiVpOrig); g_uiVpShifted = false; return; }
    } else if (g_uiShift && g_cfg.uiScope != 1) {
        bool on;
        if (g_cfg.uiScope == 0) on = (n && rtv && (uintptr_t)rtv[0] == g_uiShiftRTV);
        else { on = false; if (n && rtv && rtv[0]) { ID3D10Resource* r = nullptr; rtv[0]->GetResource(&r); on = r && (void*)r == (void*)g_eyeTex; if (r) r->Release(); } }
        if (on != g_uiShiftRT) {
            g_uiShiftRT = on;
            t_OMSetRT(d, n, rtv, dsv);
            if (g_uiVpValid && t_RSSetVP) { D3D10_VIEWPORT v = on ? UiXform(g_uiVpOrig) : g_uiVpOrig; t_RSSetVP(d, 1, &v); }
            if (g_traceDraws) LogWrite(" UI shift %s", on ? "on" : "off");
            return;
        }
    }
    t_OMSetRT(d, n, rtv, dsv);
}
static void HookDrawTable(void** vt, const char* who) {
    DWORD o2; VirtualProtect(&vt[8], 17 * 4, PAGE_EXECUTE_READWRITE, &o2);
    if (vt[8] != (void*)&Hook_DrawIndexed) { o_DrawIndexed = (DrawIndexed_t)vt[8]; vt[8] = (void*)&Hook_DrawIndexed; }
    if (vt[9] != (void*)&Hook_Draw) { o_Draw = (Draw_t)vt[9]; vt[9] = (void*)&Hook_Draw; }
    if (vt[14] != (void*)&Hook_DrawIdxInst) { o_DrawIdxInst = (DrawIdxInst_t)vt[14]; vt[14] = (void*)&Hook_DrawIdxInst; }
    if (vt[15] != (void*)&Hook_DrawInst) { o_DrawInst = (DrawInst_t)vt[15]; vt[15] = (void*)&Hook_DrawInst; }
    if (vt[24] != (void*)&Hook_OMSetRT) { o_OMSetRT = (OMSetRT_t)vt[24]; vt[24] = (void*)&Hook_OMSetRT; }
    VirtualProtect(&vt[8], 17 * 4, o2, &o2);
    LOG("draw hooks on %s vtable %p", who, (void*)vt);
}
// The engine caches D3D10 method pointers (draws bypass the device vtable). Find copies of the original method
// addresses in Dunia's data section and in the driver object, and point them at our hooks.
static void PatchCachedPointers(uint8_t* drv) {
    struct { void* orig; void* hook; const char* name; } t[] = {
        { (void*)o_DrawIndexed, (void*)&Hook_DrawIndexed, "DrawIndexed" }, { (void*)o_Draw, (void*)&Hook_Draw, "Draw" },
        { (void*)o_DrawIdxInst, (void*)&Hook_DrawIdxInst, "DrawIndexedInstanced" }, { (void*)o_DrawInst, (void*)&Hook_DrawInst, "DrawInstanced" },
        { (void*)o_OMSetRT, (void*)&Hook_OMSetRT, "OMSetRenderTargets" } };
    auto scan = [&](uint8_t* base, size_t size, const char* region) {
        MEMORY_BASIC_INFORMATION m;
        for (uint8_t* p = base; p < base + size; ) {
            if (!VirtualQuery(p, &m, sizeof m)) break;
            uint8_t* end = (uint8_t*)m.BaseAddress + m.RegionSize; if (end > base + size) end = base + size;
            bool rw = m.State == MEM_COMMIT && (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
            if (rw) for (uint32_t* q = (uint32_t*)(((uintptr_t)p + 3) & ~3); (uint8_t*)(q + 1) <= end; ++q)
                for (auto& e : t) if (e.orig && *q == (uint32_t)(uintptr_t)e.orig) { LOG("cached %s pointer at %p (%s) -> hooked", e.name, (void*)q, region); *q = (uint32_t)(uintptr_t)e.hook; }
            p = end;
        }
    };
    // whole process: every committed private read-write region (the engine keeps its D3D method cache on the heap)
    MEMORY_BASIC_INFORMATION m; uint8_t* p = (uint8_t*)0x10000;
    int regions = 0;
    while ((uintptr_t)p < 0x7FFE0000 && VirtualQuery(p, &m, sizeof m)) {
        if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(m.Protect & PAGE_GUARD)) {
            char tag[48]; snprintf(tag, sizeof tag, "region %p", m.BaseAddress);
            scan((uint8_t*)m.BaseAddress, m.RegionSize, tag); ++regions;
        }
        p = (uint8_t*)m.BaseAddress + m.RegionSize;
    }
    LOG("cached-pointer scan: %d regions", regions);
}
// ---- dynamic draw-variant hooks. The D3D10-on-11 runtime rewrites the draw slots of the device's per-instance
// vtable as pipeline state changes, so every variant that appears there gets its own inline thunk.
static int g_drawPolicy(uintptr_t* sp, int slot);
static volatile LONG g_drawSkipFlag = 0;
extern "C" int __cdecl OnDrawThunk(uintptr_t* sp, int slot) {
    NoteDraw(sp, g_vtDev);
    cbcap::OnDraw(0x200 | slot, (uint32_t)sp[2], (uint32_t)sp[3], slot == 9 ? 0 : (uint32_t)sp[4], slot >= 14 ? (uint32_t)sp[5] : 0, slot == 14 ? (uint32_t)sp[6] : 0, g_capRTV0, g_inEye ? g_capEye : 3);
    int skip = g_drawPolicy(sp, slot);
    g_drawSkipFlag = skip;
    return skip;
}
struct DrawVariant { uintptr_t fn; int slot; };
static DrawVariant g_variants[64]; static int g_nVariants = 0;
static uintptr_t g_badVariants[32]; static int g_nBad = 0;
static void HookDrawVariant(uintptr_t fn, int slot) {
    for (int i = 0; i < g_nVariants; ++i) if (g_variants[i].fn == fn) return;
    for (int i = 0; i < g_nBad; ++i) if (g_badVariants[i] == fn) return;
    static const uint8_t hot[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    const uint8_t* b = (const uint8_t*)fn;
    char where[96]; ModOff(fn, where, sizeof where);
    if (g_nVariants >= 64 || memcmp(b, hot, 5) != 0) {
        if (g_nBad < 32) g_badVariants[g_nBad++] = fn;
        LOG("draw variant slot %d at %s: prologue %02x %02x %02x %02x %02x %02x, not hooked", slot, where, b[0], b[1], b[2], b[3], b[4], b[5]);
        return;
    }
    int argBytes = slot == 8 ? 16 : slot == 9 ? 12 : slot == 14 ? 24 : 20;
    uint8_t* tr = (uint8_t*)MakeTrampoline(fn, hot, 5);
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    size_t k = 0;
    t[k++] = 0x60;                                   // pushad
    t[k++] = 0x9C;                                   // pushfd
    t[k++] = 0x6A; t[k++] = (uint8_t)slot;           // push slot
    t[k++] = 0x8D; t[k++] = 0x44; t[k++] = 0x24; t[k++] = 40;   // lea eax,[esp+40] (return address)
    t[k++] = 0x50;                                   // push eax
    t[k++] = 0xE8; *(int32_t*)(t + k) = (int32_t)((uintptr_t)&OnDrawThunk - ((uintptr_t)t + k + 4)); k += 4;
    t[k++] = 0x83; t[k++] = 0xC4; t[k++] = 0x08;     // add esp,8
    t[k++] = 0x9D;                                   // popfd
    t[k++] = 0x61;                                   // popad
    t[k++] = 0x83; t[k++] = 0x3D; *(uint32_t*)(t + k) = (uint32_t)(uintptr_t)&g_drawSkipFlag; k += 4; t[k++] = 0x00;   // cmp dword [flag],0
    t[k++] = 0x74; t[k++] = 0x03;                    // je +3
    t[k++] = 0xC2; *(uint16_t*)(t + k) = (uint16_t)argBytes; k += 2;   // ret argBytes
    t[k++] = 0xE9; *(int32_t*)(t + k) = (int32_t)((uintptr_t)tr - ((uintptr_t)t + k + 4)); k += 4;
    FlushInstructionCache(GetCurrentProcess(), t, 64);
    DWORD old; VirtualProtect((void*)fn, 5, PAGE_EXECUTE_READWRITE, &old);
    uint8_t j[5]; j[0] = 0xE9; *(int32_t*)(j + 1) = (int32_t)((uintptr_t)t - (fn + 5));
    memcpy((void*)fn, j, 5);
    VirtualProtect((void*)fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, 5);
    g_variants[g_nVariants++] = { fn, slot };
    LOG("draw variant slot %d at %s hooked (%d total)", slot, where, g_nVariants);
}
static void ScanDrawSlots() {
    if (!g_vtDev) return;
    void** vt = *(void***)g_vtDev;
    static const int slots[4] = { 8, 9, 14, 15 };
    for (int s2 : slots) {
        uintptr_t f = (uintptr_t)vt[s2];
        if (f == (uintptr_t)&Hook_DrawIndexed || f == (uintptr_t)&Hook_Draw || f == (uintptr_t)&Hook_DrawIdxInst || f == (uintptr_t)&Hook_DrawInst) continue;
        HookDrawVariant(f, s2);
    }
}
// UiScope 3: inside the UI tail, while the eye target is bound, every draw except the engine's full-screen
// composite (called from 0x10379C36) is UI and gets the per-eye UI viewport.
// Capture-only probe: after each UI draw, the bounding box of the layer pixels it changed.
static ID3D10Texture2D* g_probeSt = nullptr; static uint32_t* g_probePrev = nullptr; static int g_probeW = 0, g_probeH = 0;
static void UiProbe(int prevOrd) {
    ID3D10Texture2D* hud = xr::HudTexture(); if (!hud) return;
    D3D10_TEXTURE2D_DESC d; hud->GetDesc(&d);
    if (!g_probeSt) { D3D10_TEXTURE2D_DESC sd = d; sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1;
        if (FAILED(g_vtDev->CreateTexture2D(&sd, nullptr, &g_probeSt))) { g_probeSt = nullptr; return; }
        g_probeW = d.Width / 4; g_probeH = d.Height / 4; g_probePrev = (uint32_t*)calloc((size_t)g_probeW * g_probeH, 4); }
    g_vtDev->CopyResource(g_probeSt, hud);
    D3D10_MAPPED_TEXTURE2D m;
    if (FAILED(g_probeSt->Map(0, D3D10_MAP_READ, 0, &m))) return;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; long n = 0;
    for (int y = 0; y < g_probeH; ++y) { const uint32_t* r = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * 4 * m.RowPitch);
        for (int x = 0; x < g_probeW; ++x) { uint32_t v = r[x * 4]; uint32_t& pv = g_probePrev[(size_t)y * g_probeW + x];
            if (v != pv) { if (prevOrd >= 0) { ++n; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; } pv = v; } } }
    g_probeSt->Unmap(0);
    if (prevOrd >= 0) {
        if (n) LogWrite("  UI draw #%d changed %ld px, bbox x %.3f-%.3f y %.3f-%.3f", prevOrd, n, x0 / (float)g_probeW, (x1 + 1) / (float)g_probeW, y0 / (float)g_probeH, (y1 + 1) / (float)g_probeH);
        else LogWrite("  UI draw #%d changed nothing", prevOrd);
    }
}
// UI census: which UI draws (texture size/format, vertex count) appear and disappear, logged with what happens in the
// game at that moment, to find the draws of the interaction icon and the damage overlay
struct CensusEntry { char key[72]; double last; bool shown; };
static CensusEntry g_census[256]; static int g_nCensus = 0;
static double g_snapAt = 0, g_snapLast = -1e9; static int g_snaps = 0; static bool g_snapGame = false;
static void SnapRequest(double now) { if (g_cfg.hudSnap && g_snapGame && g_snaps < 12 && !g_snapAt && now - g_snapLast > 4000.0) g_snapAt = now + 700.0; }
static double g_loadPicMs = -1e9;   // v229: the loading screen's picture (1280x800 A8 draw) last drawn
static void UiCensus(const char* key) {
    double now = NowMs();
    if (!strncmp(key, "1280x800x65", 11)) g_loadPicMs = now;
    for (int i = 0; i < g_nCensus; ++i) if (!strcmp(g_census[i].key, key)) {
        if (!g_census[i].shown) { g_census[i].shown = true; LOG("UI census: %s appears", key); SnapRequest(now); }
        g_census[i].last = now; return;
    }
    if (g_nCensus < 256) { CensusEntry& e = g_census[g_nCensus++]; snprintf(e.key, sizeof e.key, "%s", key); e.last = now; e.shown = true; LOG("UI census: %s appears (new)", key); SnapRequest(now); }
}
static void UiCensusTick() {
    double now = NowMs();
    for (int i = 0; i < g_nCensus; ++i) if (g_census[i].shown && now - g_census[i].last > 600.0) { g_census[i].shown = false; LOG("UI census: %s gone", g_census[i].key); }
}
// Small UI textures get a content hash (first mip, read back once per texture), so textures of the same size and format
// - the interaction hand print and the shotgun reticle - can be told apart in UiSkipTex ("64x64x77v6#1a2b3c4d").
static volatile bool g_reticleOnUi = false;   // the game would show a weapon reticle now (HUD +0x188)
static uint32_t UiTexHash(ID3D10Texture2D* t, const D3D10_TEXTURE2D_DESC& td) {
    struct C { void* p; uint32_t h; }; static C cache[512]; static int n = 0, next = 0;
    for (int i = 0; i < n; ++i) if (cache[i].p == t) return cache[i].h;
    uint32_t h = 0;
    if (td.Width * td.Height <= 512 * 512 && td.SampleDesc.Count == 1) {
        D3D10_TEXTURE2D_DESC sd = td; sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D10Texture2D* st = nullptr;
        if (SUCCEEDED(g_vtDev->CreateTexture2D(&sd, nullptr, &st)) && st) {
            g_vtDev->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
            D3D10_MAPPED_TEXTURE2D m{};
            if (SUCCEEDED(st->Map(0, D3D10_MAP_READ, 0, &m)) && m.pData) {
                bool bc = td.Format >= 70 && td.Format <= 84;   // block-compressed: rows of 4x4 blocks
                UINT rows = bc ? (td.Height + 3) / 4 : td.Height;
                h = 2166136261u;
                const uint8_t* p = (const uint8_t*)m.pData;
                for (UINT y = 0; y < rows; ++y) { const uint8_t* row = p + (size_t)y * m.RowPitch; for (UINT x = 0; x < m.RowPitch; x += 4) { h ^= *(const uint32_t*)(row + x); h *= 16777619u; } }
                st->Unmap(0);
            }
            st->Release();
        }
    }
    C& c = n < 512 ? cache[n++] : cache[next++ % 512]; c.p = t; c.h = h;
    return h;
}
static int g_drawPolicy(uintptr_t* sp, int slot) {
    if (g_cfg.uiMode == 2) {
        if (!g_uiShift || !g_uiOnEye || !g_uiLayerRTV) return 0;
        uint32_t c[2] = { 0, 0 }; int found = 0;
        for (int i = 0; i < 300 && found < 2; ++i) { uintptr_t v = sp[i]; if (v > g_base + 0x1000 && v < g_base + 0xE00000) c[found++] = (uint32_t)(v - g_base + 0x10000000); }
        if (c[1] == V(0x10379C36)) return 0;                 // full-screen scene composite
        int ord = g_uiDrawOrd++;
        if (g_traceDraws && g_uiCurEye == g_uiPrimary) {
            ID3D10ShaderResourceView* srv = nullptr; g_vtDev->PSGetShaderResources(0, 1, &srv);
            char d[96] = "none";
            if (srv) { ID3D10Resource* r = nullptr; srv->GetResource(&r); ID3D10Texture2D* t = nullptr;
                if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) { D3D10_TEXTURE2D_DESC td; t->GetDesc(&td); snprintf(d, sizeof d, "tex %p %ux%u fmt=%d", (void*)t, td.Width, td.Height, (int)td.Format); t->Release(); }
                if (r) r->Release(); srv->Release(); }
            ID3D10BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; g_vtDev->OMGetBlendState(&bs, bf, &sm);
            int wm = -1, be = -1; if (bs) { D3D10_BLEND_DESC bd; bs->GetDesc(&bd); wm = bd.RenderTargetWriteMask[0]; be = bd.BlendEnable[0]; bs->Release(); }
            LogWrite("  UI draw #%d slot %d args %u %u  ps0: %s  blend %d mask %x", ord, slot, (unsigned)sp[2], (unsigned)sp[3], d, be, wm);
        }
        if (g_uiCurEye != g_uiPrimary) return 1;           // the other eye skips the UI
        if (g_capState == 2) UiProbe(ord - 1);
        // Reticle: drawn with a tiny texture (8x16 in the default crosshair).
        bool ret = false, skip = false;
        {
            ID3D10ShaderResourceView* srv = nullptr; g_vtDev->PSGetShaderResources(0, 1, &srv);
            if (srv) { ID3D10Resource* r = nullptr; srv->GetResource(&r); ID3D10Texture2D* t = nullptr;
                if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t))) {
                    D3D10_TEXTURE2D_DESC td; t->GetDesc(&td); ret = !g_scopeBBres && g_uiRetRTV && (int)(td.Width * td.Height) <= g_cfg.reticleTexMax;   // in the scope view the reticle stays with the UI
                    char key[48]; snprintf(key, sizeof key, "%ux%ux%dv%u,", td.Width, td.Height, (int)td.Format, (unsigned)sp[2]);
                    uint32_t th = (td.Width * td.Height <= 512 * 512) ? UiTexHash(t, td) : 0;
                    char keyH[64]; snprintf(keyH, sizeof keyH, "%ux%ux%dv%u#%08x%s", td.Width, td.Height, (int)td.Format, (unsigned)sp[2], th, g_reticleOnUi ? " (reticle on)" : "");
                    UiCensus(keyH);
                    char list[520]; snprintf(list, sizeof list, ",%s,", g_cfg.uiSkipTex);
                    char k2[52]; snprintf(k2, sizeof k2, ",%s", key);   // key already ends with ','
                    char k3[64]; snprintf(k3, sizeof k3, ",%ux%ux%dv%u#%08x,", td.Width, td.Height, (int)td.Format, (unsigned)sp[2], th);
                    char k4[64]; snprintf(k4, sizeof k4, ",%ux%ux%dv%u@r,", td.Width, td.Height, (int)td.Format, (unsigned)sp[2]);   // only while a weapon reticle is up
                    if (g_cfg.uiSkipTex[0] && (strstr(list, k2) || strstr(list, k3) || (g_reticleOnUi && strstr(list, k4)))) skip = true;
                    t->Release(); }
                if (r) r->Release(); srv->Release(); }
        }
        if (skip) return 1;     // draws that only put an opaque black box into the transparent layer
        if (ret && g_cfg.reticleDot && controls::Active()) return 1;   // the game's crosshair is replaced by the aiming dot
        ID3D10RenderTargetView* want = ret ? g_uiRetRTV : g_uiLayerRTV;
        if (!g_uiRedirect) {
            if (g_uiSavedEyeRTV) { g_uiSavedEyeRTV->Release(); g_uiSavedEyeRTV = nullptr; }
            if (g_uiSavedEyeDSV) { g_uiSavedEyeDSV->Release(); g_uiSavedEyeDSV = nullptr; }
            g_vtDev->OMGetRenderTargets(1, &g_uiSavedEyeRTV, &g_uiSavedEyeDSV);
            g_uiBoundLayer = nullptr; g_uiRedirect = true;
        }
        if (g_uiBoundLayer != want) { t_OMSetRT(g_vtDev, 1, &want, g_uiSavedEyeDSV); g_uiBoundLayer = want; }
        if (ret) g_uiRetFresh = true;
        // alpha fix for additive draws
        {
            ID3D10BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; g_vtDev->OMGetBlendState(&bs, bf, &sm);
            if (bs && bs == g_blendSetFixed) { bs->Release(); bs = nullptr; }   // still our replacement from the previous draw
            else if (g_blendSetFixed) g_blendSetFixed = nullptr;
            if (bs) {
                ID3D10BlendState* fixedBs = nullptr;
                for (int i = 0; i < g_nBlendFix; ++i) if (g_blendFix[i].orig == bs) { fixedBs = g_blendFix[i].fixed; break; }
                if (!fixedBs && g_nBlendFix < 32) {
                    D3D10_BLEND_DESC bd; bs->GetDesc(&bd);
                    LOG("UI layer: blend state %p: enable %d src %d dst %d op %d srcA %d dstA %d opA %d mask %x", (void*)bs, bd.BlendEnable[0], bd.SrcBlend, bd.DestBlend, bd.BlendOp, bd.SrcBlendAlpha, bd.DestBlendAlpha, bd.BlendOpAlpha, bd.RenderTargetWriteMask[0]);
                    bool srcNormal = bd.SrcBlend == D3D10_BLEND_ONE || bd.SrcBlend == D3D10_BLEND_SRC_ALPHA;
                    if (bd.BlendEnable[0] && (bd.DestBlend == D3D10_BLEND_ONE || !srcNormal) && (bd.RenderTargetWriteMask[0] & 8)) {
                        bd.RenderTargetWriteMask[0] &= 7;
                        if (FAILED(g_vtDev->CreateBlendState(&bd, &fixedBs))) fixedBs = nullptr;
                    }
                    bs->AddRef(); g_blendFix[g_nBlendFix++] = { bs, fixedBs };
                    if (fixedBs) LOG("UI layer: blend state %p -> RGB-only copy", (void*)bs);
                }
                if (fixedBs) { g_vtDev->OMSetBlendState(fixedBs, bf, sm); g_blendSetFixed = fixedBs; g_blendSetOrig = bs; }
                bs->Release();
            }
        }
        return 0;
    }
    (void)slot;
    if (!g_uiShift || g_cfg.uiScope != 3 || !g_uiVpValid || !t_RSSetVP) return 0;
    bool want = false;
    if (g_uiOnEye) {
        uint32_t c[2] = { 0, 0 }; int found = 0;
        for (int i = 0; i < 300 && found < 2; ++i) { uintptr_t v = sp[i]; if (v > g_base + 0x1000 && v < g_base + 0xE00000) c[found++] = (uint32_t)(v - g_base + 0x10000000); }
        want = c[1] != V(0x10379C36);
    }
    if (want != g_uiVpShifted) {
        D3D10_VIEWPORT v = want ? UiXform(g_uiVpOrig) : g_uiVpOrig;
        t_RSSetVP(g_vtDev, 1, &v);
        g_uiVpShifted = want;
    }
    return 0;
}
static void InlineHookImpl(void* fn, void* hook, void** tramp, const char* name) {
    static const uint8_t hot[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    uint8_t* p = (uint8_t*)fn;
    if (!p) return;
    if (memcmp(p, hot, 5) != 0) { LOG("inline %s: prologue %02x %02x %02x %02x %02x %02x, not hooked", name, p[0], p[1], p[2], p[3], p[4], p[5]); return; }
    bool ok = PatchJmpN((uintptr_t)p, hot, 5, hook, tramp);
    char where[96]; ModOff((uintptr_t)p, where, sizeof where);
    LOG("inline %s at %s: %d", name, where, ok);
}

// ---------------------------------------------------------------- v232 arm jitter: GPU constant capture (Ctrl+F4)
// Records 3 consecutive frames: every constant buffer write (Map/Unmap, UpdateSubresource), VS constant-buffer and
// shader bindings, every draw (with the bound VS CBs, render target, eye), and the arm skeleton the mod wrote (from the
// game thread). Written to fc2vr_cbcap.bin with the bytecode of the vertex shaders used, for offline analysis.
namespace cbcap {
typedef void (STDMETHODCALLTYPE *VSSetCB_t)(ID3D10Device*, UINT, UINT, ID3D10Buffer* const*);
typedef void (STDMETHODCALLTYPE *VSSetShader_t)(ID3D10Device*, ID3D10VertexShader*);
typedef void (STDMETHODCALLTYPE *UpdSub_t)(ID3D10Device*, ID3D10Resource*, UINT, const D3D10_BOX*, const void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateVS_t)(ID3D10Device*, const void*, SIZE_T, ID3D10VertexShader**);
typedef HRESULT (STDMETHODCALLTYPE *BufMap_t)(ID3D10Buffer*, D3D10_MAP, UINT, void**);
typedef void (STDMETHODCALLTYPE *BufUnmap_t)(ID3D10Buffer*);
static VSSetCB_t t_VSSetCB; static VSSetShader_t t_VSSetShader; static UpdSub_t t_UpdSub; static CreateVS_t o_CreateVS; static BufMap_t t_Map; static BufUnmap_t t_Unmap;
static CRITICAL_SECTION cs; static bool csInit; static volatile int state;   // 0 idle, 1 armed (starts at next present), 2 capturing
static int frames; static std::string out; static ID3D10Buffer* cb[16]; static void* curVS;
struct MapRec { ID3D10Buffer* b; void* p; }; static MapRec maps[64];
struct VsCode { void* vs; std::string code; }; static std::vector<VsCode> vsCodes; static CRITICAL_SECTION vsCs; static bool vsInit;
static void Put(char type, const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0) {
    if (out.size() > (64u << 20)) return;
    uint32_t len = na + nb; out.push_back(type); out.append((const char*)&len, 4); out.append((const char*)a, na); if (b && nb) out.append((const char*)b, nb);
}
static bool lite = false;   // Ctrl+Shift+F4 motion capture: per-frame poses + skeleton only (540 frames)
static bool On() { return state == 2 && !lite; }
float mx[96]; bool mxValid = false;   // this frame's 'X' record (render thread)
static void STDMETHODCALLTYPE I_VSSetCB(ID3D10Device* d, UINT start, UINT n, ID3D10Buffer* const* b) {
    if (b) for (UINT i = 0; i < n && start + i < 16; ++i) cb[start + i] = b[i];
    if (On()) { EnterCriticalSection(&cs); uint32_t h[2 + 16] = { start, n }; for (UINT i = 0; i < n && i < 16; ++i) h[2 + i] = (uint32_t)(uintptr_t)(b ? b[i] : nullptr); Put('S', h, 8 + 4 * (n < 16 ? n : 16)); LeaveCriticalSection(&cs); }
    t_VSSetCB(d, start, n, b);
}
static void STDMETHODCALLTYPE I_VSSetShader(ID3D10Device* d, ID3D10VertexShader* vs) { curVS = vs; t_VSSetShader(d, vs); }
static void BufData(ID3D10Buffer* b, const void* data) {
    D3D10_BUFFER_DESC bd; b->GetDesc(&bd); if (!(bd.BindFlags & D3D10_BIND_CONSTANT_BUFFER) || bd.ByteWidth > 65536) return;
    uint32_t h[2] = { (uint32_t)(uintptr_t)b, bd.ByteWidth }; EnterCriticalSection(&cs); Put('B', h, 8, data, bd.ByteWidth); LeaveCriticalSection(&cs);
}
static void STDMETHODCALLTYPE I_UpdSub(ID3D10Device* d, ID3D10Resource* r, UINT sub, const D3D10_BOX* box, const void* data, UINT rp, UINT dp) {
    if (On() && !box && data && r) { D3D10_RESOURCE_DIMENSION dim; r->GetType(&dim); if (dim == D3D10_RESOURCE_DIMENSION_BUFFER) BufData((ID3D10Buffer*)r, data); }
    t_UpdSub(d, r, sub, box, data, rp, dp);
}
static HRESULT STDMETHODCALLTYPE I_Map(ID3D10Buffer* b, D3D10_MAP t, UINT f, void** pp) {
    HRESULT hr = t_Map(b, t, f, pp);
    if (On() && SUCCEEDED(hr) && pp && *pp && t != D3D10_MAP_READ) { EnterCriticalSection(&cs); for (auto& m : maps) if (!m.b || m.b == b) { m.b = b; m.p = *pp; break; } LeaveCriticalSection(&cs); }
    return hr;
}
static void STDMETHODCALLTYPE I_Unmap(ID3D10Buffer* b) {
    if (On()) { void* p = nullptr; EnterCriticalSection(&cs); for (auto& m : maps) if (m.b == b) { p = m.p; m.b = nullptr; break; } LeaveCriticalSection(&cs); if (p) BufData(b, p); }
    t_Unmap(b);
}
static HRESULT STDMETHODCALLTYPE H_CreateVS(ID3D10Device* d, const void* code, SIZE_T n, ID3D10VertexShader** out_) {
    HRESULT hr = o_CreateVS(d, code, n, out_);
    if (SUCCEEDED(hr) && out_ && *out_ && code && n < 262144) { if (!vsInit) { InitializeCriticalSection(&vsCs); vsInit = true; } EnterCriticalSection(&vsCs); vsCodes.push_back({ *out_, std::string((const char*)code, n) }); LeaveCriticalSection(&vsCs); }
    return hr;
}
void NoteBuffer(ID3D10Buffer* b) {   // first constant buffer created: hook Map/Unmap on the runtime implementation
    static bool done = false; if (done || !b) return; done = true;
    void** vt = *(void***)b;
    InlineHookImpl(vt[10], (void*)&I_Map, (void**)&t_Map, "ID3D10Buffer::Map");
    InlineHookImpl(vt[11], (void*)&I_Unmap, (void**)&t_Unmap, "ID3D10Buffer::Unmap");
}
void Install(void** vt) {
    if (!csInit) { InitializeCriticalSection(&cs); csInit = true; }
    if (!vsInit) { InitializeCriticalSection(&vsCs); vsInit = true; }
    InlineHookImpl(vt[3], (void*)&I_VSSetCB, (void**)&t_VSSetCB, "VSSetConstantBuffers");
    InlineHookImpl(vt[7], (void*)&I_VSSetShader, (void**)&t_VSSetShader, "VSSetShader");
    InlineHookImpl(vt[34], (void*)&I_UpdSub, (void**)&t_UpdSub, "UpdateSubresource");
}
void HookCreateVS(void** vt) { DWORD o; VirtualProtect(&vt[79], 4, PAGE_EXECUTE_READWRITE, &o); o_CreateVS = (CreateVS_t)vt[79]; vt[79] = (void*)&H_CreateVS; VirtualProtect(&vt[79], 4, o, &o); }
static long drawCalls[2];
void OnDraw(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t f, void* rtv, int eye) {
    if (!On()) return;
    InterlockedIncrement(&drawCalls[(kind & 0x300) ? 1 : 0]);
    uint32_t h[8 + 8] = { kind, a, b, c, e, f, (uint32_t)(uintptr_t)curVS, (uint32_t)(uintptr_t)rtv | ((uint32_t)(eye & 3) << 30) };
    for (int i = 0; i < 8; ++i) h[8 + i] = (uint32_t)(uintptr_t)cb[i];
    EnterCriticalSection(&cs); Put('D', h, sizeof h); LeaveCriticalSection(&cs);
}
void NoteBones(const float* W, const float* models, int n) {   // game thread, after the arm write
    if (state != 2 && state != 1) return;
    uint32_t h[2] = { GetCurrentThreadId(), (uint32_t)n }; EnterCriticalSection(&cs);
    std::string buf((const char*)h, 8); buf.append((const char*)W, 64); buf.append((const char*)models, n * 64);
    if (lite) { float p[3], fw[3]; uint64_t sq = 0; arms::DiagApplied(p, fw, &sq); double t = NowMs(); buf.append((const char*)&sq, 8); buf.append((const char*)p, 12); buf.append((const char*)fw, 12); buf.append((const char*)&t, 8); }
    Put('K', buf.data(), (uint32_t)buf.size()); LeaveCriticalSection(&cs);
}
void Arm(bool mocap) { if (state == 0) { lite = mocap; state = 1; LOG(mocap ? "mocap: armed (540 frames)" : "cbcap: armed (3 frames)"); ShowToast(mocap ? "Motion capture: 6 s" : "GPU capture: 3 frames"); } }
void Present(const char* iniDir) {   // render thread, each present
    if (state == 1) { EnterCriticalSection(&cs); out.clear(); frames = 0; for (auto& m : maps) m.b = nullptr; LeaveCriticalSection(&cs); state = 2; }
    if (state != 2) return;
    double t = NowMs(); uint32_t h[3] = { (uint32_t)frames, 0, 0 }; memcpy(&h[1], &t, 8);
    EnterCriticalSection(&cs); Put('F', h, 12); if (lite && mxValid) Put('X', mx, sizeof mx); mxValid = false; LeaveCriticalSection(&cs);
    if (++frames <= (lite ? 540 : 3)) return;
    state = 3;
    // vertex shaders used
    std::vector<void*> used; size_t i = 0;
    while (i + 5 <= out.size()) { char ty = out[i]; uint32_t len; memcpy(&len, &out[i + 1], 4); if (ty == 'D' && len >= 28) { uint32_t vs; memcpy(&vs, &out[i + 5 + 24], 4); bool k = false; for (auto u : used) if (u == (void*)(uintptr_t)vs) k = true; if (!k) used.push_back((void*)(uintptr_t)vs); } i += 5 + len; }
    EnterCriticalSection(&vsCs);
    for (auto& v : vsCodes) for (auto u : used) if (u == v.vs) { uint32_t p = (uint32_t)(uintptr_t)v.vs; Put('C', &p, 4, v.code.data(), (uint32_t)v.code.size()); }
    LeaveCriticalSection(&vsCs);
    char path[MAX_PATH]; snprintf(path, sizeof path, lite ? "%sfc2vr_mocap.bin" : "%sfc2vr_cbcap.bin", iniDir);
    FILE* fp = fopen(path, "wb"); if (fp) { fwrite(out.data(), 1, out.size(), fp); fclose(fp); }
    LOG("cbcap: %u bytes written to %s (%u vertex shaders; draws seen: inline %ld, vtable/variant %ld)", (unsigned)out.size(), path, (unsigned)used.size(), drawCalls[0], drawCalls[1]); drawCalls[0] = drawCalls[1] = 0;
    ShowToast(lite ? "Motion capture saved" : "GPU capture saved");
    out.clear(); out.shrink_to_fit(); state = 0;
}
}

// ---------------------------------------------------------------- v232 frame pacing
// The game's frame timer (global timer object at 0x11606360, Update 0x1029B290) measures each frame's length from the
// wall clock. In VR the frames are shown at a fixed rate, but when the game thread wakes varies by +-1 ms, so the player
// (and every vehicle) moved by uneven steps: at 5 m/s +-3.5 mm per frame, in a taxi at 11 m/s +-6 mm - the world and
// the car judder past the view. With FramePacing=1 the measured step is replaced by a whole number of display periods;
// the difference is kept as a debt and paid back a little each frame so game time never drifts from real time.
// Layout: +0x18 last real time, +0x28 game time, +0x30 unpaused time, +0x38 scaled dt, +0x40 dt, +0x48 paused,
// +0x50 time scale, +0x60 fixed-step mode (left alone).
typedef void (__fastcall *TimerUpdate_t)(uint8_t*, void*);
static TimerUpdate_t o_TimerUpdate = nullptr;
static int g_framePacing = 1; static double g_paceDebt = 0; static long g_paceFrames = 0, g_paceSkipped = 0;
static void __fastcall Hook_TimerUpdate(uint8_t* t, void* edx) {
    o_TimerUpdate(t, edx);
    uintptr_t gp = A(0x11606360);
    if (!g_framePacing || !gp || t != *(uint8_t**)gp || t[0x60]) return;
    double P = xr::DisplayPeriod();
    if (P < 0.004 || P > 0.05) return;
    double& dtRaw = *(double*)(t + 0x40); double& dtScaled = *(double*)(t + 0x38);
    double raw = dtRaw, n = floor(raw / P + 0.5); if (n < 1) n = 1;
    if (raw <= 0 || raw > 0.25 || fabs(raw - n * P) > 0.3 * P) { g_paceDebt = 0; ++g_paceSkipped; return; }   // irregular frame: as measured
    double dt = n * P; g_paceDebt += raw - dt;
    double pay = g_paceDebt * 0.05; if (pay > 0.0003) pay = 0.0003; if (pay < -0.0003) pay = -0.0003;
    if (fabs(g_paceDebt) > 0.03) pay = g_paceDebt;   // far off (a hitch): settle at once
    dt += pay; g_paceDebt -= pay;
    double scale = *(double*)(t + 0x50), ns = dt * scale, d = ns - dtScaled;
    *(double*)(t + 0x28) += d; if (!t[0x48]) *(double*)(t + 0x30) += d;
    dtScaled = ns; dtRaw = dt; ++g_paceFrames;
    static double lastL = 0; double tn = NowMs();
    if (tn - lastL > 30000.0) { lastL = tn; LOG("frame pacing: %ld frames paced, %ld left as measured, debt %.2f ms, period %.3f ms", g_paceFrames, g_paceSkipped, g_paceDebt * 1000.0, P * 1000.0); g_paceFrames = g_paceSkipped = 0; }
}
static void InstallFramePacing() {
    static const uint8_t pro[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    bool ok = PatchJmpN(A(0x1029B290), pro, 6, (void*)&Hook_TimerUpdate, (void**)&o_TimerUpdate);
    LOG("frame pacing hook (game timer update): %s, FramePacing=%d", ok ? "ok" : "FAILED", g_framePacing);
}
static void InstallInlineDrawHooks() {
    static bool done = false; if (done) return; done = true;
    InlineHookImpl((void*)o_DrawIndexed, (void*)&I_DrawIndexed, (void**)&t_DrawIndexed, "DrawIndexed");
    InlineHookImpl((void*)o_Draw, (void*)&I_Draw, (void**)&t_Draw, "Draw");
    InlineHookImpl((void*)o_DrawIdxInst, (void*)&I_DrawIdxInst, (void**)&t_DrawIdxInst, "DrawIndexedInstanced");
    InlineHookImpl((void*)o_DrawInst, (void*)&I_DrawInst, (void**)&t_DrawInst, "DrawInstanced");
    InlineHookImpl((void*)o_OMSetRT, (void*)&I_OMSetRT, (void**)&t_OMSetRT, "OMSetRenderTargets");
    if (o_RSSetVP) InlineHookImpl((void*)o_RSSetVP, (void*)&I_RSSetVP, (void**)&t_RSSetVP, "RSSetViewports");
    if (g_vtDev) cbcap::Install(*(void***)g_vtDev);
    arms::SetBoneSink(&cbcap::NoteBones);
}
// DLSS upscaling: the eyes render smaller than shown, so texture detail is picked one or more mip levels too blurry.
// While an eye renders, every pixel-shader sampler is swapped for a copy with MipLODBias += log2(render / output)
// (NVIDIA's DLSS guide); shadow-map comparison samplers and samplers without mips are left alone.
typedef void (STDMETHODCALLTYPE *PSSetSamplers_t)(ID3D10Device*, UINT, UINT, ID3D10SamplerState* const*);
typedef HRESULT (STDMETHODCALLTYPE *CreateSampler_t)(ID3D10Device*, const D3D10_SAMPLER_DESC*, ID3D10SamplerState**);
static PSSetSamplers_t o_PSSetSamplers = nullptr; static CreateSampler_t o_CreateSampler = nullptr;
static float g_lodBias = 0.0f;
struct SampMap { ID3D10SamplerState* src; ID3D10SamplerState* biased; };
static SampMap g_samp[512]; static int g_nSamp = 0;
static ID3D10SamplerState* BiasedSampler(ID3D10Device* d, ID3D10SamplerState* s) {
    if (!s) return s;
    uint32_t h = (uint32_t)(((uintptr_t)s >> 4) * 2654435761u) & 511;
    for (int i = 0; i < 512; ++i) { SampMap& m = g_samp[(h + i) & 511]; if (m.src == s) return m.biased ? m.biased : s; if (!m.src) break; }
    D3D10_SAMPLER_DESC sd; s->GetDesc(&sd);
    ID3D10SamplerState* b = nullptr;
    if (!(sd.Filter & 0x80) && sd.MaxLOD > sd.MinLOD) {
        sd.MipLODBias += g_lodBias;
        if (FAILED(o_CreateSampler(d, &sd, &b))) b = nullptr;
    }
    if (g_nSamp < 448) {
        for (int i = 0; i < 512; ++i) { SampMap& m = g_samp[(h + i) & 511]; if (!m.src) { s->AddRef(); m.src = s; m.biased = b; ++g_nSamp; break; } }
        static int logged = 0; if (logged < 1 && g_nSamp >= 8) { ++logged; LOG("DLSS upscaling: texture LOD bias %.2f on the eye render's samplers", g_lodBias); }
    } else if (b) { b->Release(); b = nullptr; }
    return b ? b : s;
}
static void STDMETHODCALLTYPE Hook_PSSetSamplers(ID3D10Device* d, UINT start, UINT n, ID3D10SamplerState* const* ss) {
    if (g_inEye && g_lodBias < 0 && ss && n && n <= 16) {
        ID3D10SamplerState* rep[16];
        for (UINT i = 0; i < n; ++i) rep[i] = BiasedSampler(d, ss[i]);
        o_PSSetSamplers(d, start, n, rep); return;
    }
    o_PSSetSamplers(d, start, n, ss);
}
static void HookDeviceVtable(ID3D10Device* dev) {
    g_vtDev = dev;
    void** vt = *(void***)dev;
    DWORD old; VirtualProtect(&vt[71], 6 * 4, PAGE_EXECUTE_READWRITE, &old);
    o_CreateBuffer = (CreateBuffer_t)vt[71]; vt[71] = (void*)&Hook_CreateBuffer;
    cbcap::HookCreateVS(vt);
    { DWORD o2; VirtualProtect(&vt[8], 17 * 4, PAGE_EXECUTE_READWRITE, &o2);
      o_DrawIndexed = (DrawIndexed_t)vt[8]; vt[8] = (void*)&Hook_DrawIndexed;
      o_Draw = (Draw_t)vt[9]; vt[9] = (void*)&Hook_Draw;
      o_OMSetRT = (OMSetRT_t)vt[24]; vt[24] = (void*)&Hook_OMSetRT;
      o_DrawIdxInst = (DrawIdxInst_t)vt[14]; vt[14] = (void*)&Hook_DrawIdxInst;
      o_DrawInst = (DrawInst_t)vt[15]; vt[15] = (void*)&Hook_DrawInst;
      VirtualProtect(&vt[8], 17 * 4, o2, &o2); }
    o_RSSetVP = (RSSetVP_t)vt[30];
    { DWORD o3; VirtualProtect(&vt[6], 4, PAGE_EXECUTE_READWRITE, &o3); o_PSSetSamplers = (PSSetSamplers_t)vt[6]; vt[6] = (void*)&Hook_PSSetSamplers; VirtualProtect(&vt[6], 4, o3, &o3);
      VirtualProtect(&vt[86], 4, PAGE_EXECUTE_READWRITE, &o3); o_CreateSampler = (CreateSampler_t)vt[86]; VirtualProtect(&vt[86], 4, o3, &o3); }
    o_CreateTexture2D = (CreateTexture2D_t)vt[73]; vt[73] = (void*)&Hook_CreateTexture2D;
    o_CreateRTV = (CreateRTV_t)vt[76]; vt[76] = (void*)&Hook_CreateRTV;
    VirtualProtect(&vt[71], 6 * 4, old, &old);
    LOG("device vtable hooked (CreateTexture2D, CreateRenderTargetView)");
}
static int g_traceLeft = 0;   // event trace for a few frames
static void Trace(const char* ev) { hang::Mark(ev); if (g_traceLeft > 0) LogWrite("TRACE %-10s tid=%lu t=%.3f", ev, GetCurrentThreadId(), NowMs()); }
static double g_tPresentEnd = 0, g_tRFStart = 0, g_tHookStart = 0, g_tHookEnd = 0, g_tRFEnd = 0;
struct Sections { double sim = 0, rfPre = 0, hook = 0, rfPost = 0, mid = 0, present = 0; int n = 0; } g_sec;
static void SecReport() {
    Sections& s = g_sec; if (!s.n) return; double k = 1.0 / s.n;
    { static int rep = 0; if (!g_cfg.profile && rep++ % 30 != 0) { s = Sections{}; return; } }   // v225 release: every ~30 s unless Profile=1
    LOG("SECTIONS avg %d frames (ms): sim=%.2f rfPre=%.2f hook=%.2f rfPost=%.2f rfEnd->present=%.2f present=%.2f",
        s.n, s.sim * k, s.rfPre * k, s.hook * k, s.rfPost * k, s.mid * k, s.present * k);
    s = Sections{};
}
static void ProfReport() {
    Prof& p = g_prof; if (p.n == 0) return;
    double k = 1.0 / p.n;
    static int profRep = 0; const bool quiet = !g_cfg.profile && profRep++ % 30 != 0;   // v225 release: every ~30 s unless Profile=1
    if (!quiet) LOG("PROF avg over %d frames (ms): frame=%.2f wait=%.2f cams=%.2f eye0=%.2f eye1=%.2f copies+sync=%.2f end=%.2f present=%.2f | texCreates=%ld rtvCreates=%ld bufCreates=%ld (%.1f MB) tree calls=%ld rebuilds=%ld skipped=%ld treeMakes=%ld (in eye %ld, %.2f ms/frame) cacheHits=%ld nearProjEye=%ld uiBegins=%ld",
        p.n, p.frame * k, p.wait * k, p.cams * k, p.eye0 * k, p.eye1 * k, p.copies * k, p.end * k, p.present * k, g_texCreates, g_rtvCreates, g_bufCreates, g_bufBytes / 1048576.0, g_treeCalls, g_treeRebuilds, g_treeSkipped, g_treeMakes, g_treeMakesEye, g_treeMakeMs / p.n, g_treeCacheHits, g_nearProjEye, g_uiBegins);
    g_nearProjEye = 0; g_uiBegins = 0;
    g_treeMakeMs = 0;
    g_treeMakes = g_treeMakesEye = 0; g_treeCacheHits = 0;
    g_treeCalls = g_treeRebuilds = g_treeSkipped = 0;
    InterlockedExchange(&g_bufCreates, 0); g_bufBytes = 0;
    { static int rep = 0; if (g_cfg.profile && ++rep % 10 == 0) {
        std::sort(g_bufSites, g_bufSites + 128, [](const BufSite& a, const BufSite& b) { return a.n > b.n; });
        LogWrite("CreateBuffer sites over last ~900 frames:");
        for (int i = 0; i < 12 && g_bufSites[i].n; ++i) { auto& e = g_bufSites[i];
            LogWrite("  n=%u (%.1f/frame) avg=%u B usage=%d bind=%x  %08x <- %08x <- %08x <- %08x", e.n, e.n / 900.0, (unsigned)(e.bytes / e.n), e.usage, e.bind, e.c[0], e.c[1], e.c[2], e.c[3]); }
        memset(g_bufSites, 0, sizeof g_bufSites);
    } }
    InterlockedExchange(&g_texCreates, 0); InterlockedExchange(&g_rtvCreates, 0);
    p = Prof{};
}

// ============================================================ hooks
typedef void (__fastcall *RenderViewport_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (__fastcall *Present_t)(void*, void*, int);
typedef void (__fastcall *RenderFrame_t)(void*);
static RenderFrame_t o_RenderFrame = nullptr;
// ---- Bouncing NPCs fix (same result as the "Fix Bouncing NPCs" mod, done in code instead of data files).
// That mod sets CCharacterPhysComponent.CharacterParams.bUseRigidBased (+0xC2) to 0 for every human in both
// worlds' entity libraries and keeps it 1 for animals - exactly the archetypes with bUpdateRotation (+0xC0) set.
// The controller factory at 0x10493CA0 tests [params+0xC2]; testing +0xC0 instead gives the same per-archetype
// result for every entity library (DLC included). It also limits the physics and job thread counts to 1.
typedef int (__fastcall *IdealThreads_t)(void*, void*, const char*);
static IdealThreads_t o_IdealThreads = nullptr;
static int __fastcall Hook_IdealThreads(void* self, void* edx, const char* name) {
    int n = o_IdealThreads(self, edx, name);
    // the physics threads are what the bouncing-NPC fix needs limited; the job threads run the scene culling (shadow
    // cascades, visibility) - with one job thread the render thread ends up doing most of it itself (NpcFixJobThreads)
    if (name && (!strcmp(name, "PHYSIC_THREADS") || (g_cfg.npcFixJobThreads && !strcmp(name, "JOB_THREADS")))) { LOG("thread count %s: %d -> 1 (NPC fix)", name, n); return 1; }
    if (name && !strcmp(name, "JOB_THREADS")) LOG("thread count %s: %d (left to the game)", name, n);
    return n;
}
// ---- Developer console (same patch as Far Cry 2 Multi Fixer's "devmodeon"): the console command dispatcher call at
// 0x1029741B -> 0x10291D70 (thiscall, arg = std::string) is redirected; "devmodeon"/"devmodeoff" set the dev-mode byte at
// this+0x68, which unlocks commands such as domino_<weapon>, cheat_GodMode, env_hour. DevConsole=2 turns it on at once.
typedef char (__fastcall *ConsoleCmd_t)(void*, void*, uint8_t*);
static ConsoleCmd_t o_ConsoleCmd = nullptr;
static char __fastcall Hook_ConsoleCmd(void* self, void* edx, uint8_t* str) {
    if (self && str) {
        const char* s = *(uint32_t*)(str + 0x18) < 0x10 ? (const char*)(str + 4) : *(const char**)(str + 4);
        if (s) {
            if (!_stricmp(s, "devmodeon")) { *((uint8_t*)self + 0x68) = 1; LOG("console: dev mode on"); }
            else if (!_stricmp(s, "devmodeoff")) { *((uint8_t*)self + 0x68) = 0; LOG("console: dev mode off"); }
            else if (g_cfg.devConsole >= 2) *((uint8_t*)self + 0x68) = 1;
        }
    }
    return o_ConsoleCmd(self, edx, str);
}
static void InstallDevConsole() {
    if (!g_cfg.devConsole) return;
    uint8_t* site = (uint8_t*)A(0x1029741B);
    uintptr_t target = A(0x10291D70);
    if (site[0] != 0xE8 || (uintptr_t)(site + 5) + *(int32_t*)(site + 1) != target) { LOG("dev console: unexpected call site, not applied"); return; }
    o_ConsoleCmd = (ConsoleCmd_t)target;
    DWORD old; VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)&Hook_ConsoleCmd - (uintptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOG("dev console: devmodeon/devmodeoff enabled%s", g_cfg.devConsole >= 2 ? " (dev mode on with any command)" : "");
}
// Water reflection camera. The planar water reflection (WaterReflectionRenderer, 0x103A8640) copies the viewport's
// culling camera and rebuilds its projection from the scalar FOV/aspect. Cull mode 1 gives that camera the game's flat
// FOV for LOD, so the reflection only covered the middle of the VR view (streaks at the sides, reflections lost when
// looking up/down). The copy is redirected here and gets the wide VR values back; everything else keeps the flat ones.
static volatile bool g_reflWide = false; static float g_reflFov = 0, g_reflAsp = 0; static int g_reflLogged = 0;
typedef void* (__fastcall *CamCopy_t)(void* self, void* edx, void* src);
static void* __fastcall Hook_ReflCamCopy(void* self, void* edx, void* src) {
    void* r = ((CamCopy_t)A(0x1039FEA0))(self, edx, src);
    if (g_reflWide && g_reflFov > 0) {
        uint8_t* c = (uint8_t*)self;
        if (g_reflLogged < 4) { ++g_reflLogged; LOG("%s camera copy: fov %.3f aspect %.3f -> %.3f %.3f", (uintptr_t)__builtin_return_address(0) == A(0x103AE09D) ? "sun shadow" : "water reflection", F<float>(c, 0x228), F<float>(c, 0x18), g_reflFov, g_reflAsp); }
        F<float>(c, 0x228) = g_reflFov; F<float>(c, 0x18) = g_reflAsp;   // cam::Fov / cam::Aspect (rebuilt by the caller)
    }
    return r;
}
// Sun shadow cascades (ShadowRenderer 0x103AE000): the caster-culling camera is another copy of the culling camera
// (0x103AE098), and the cascade split spheres (0x103AD0D0) are fitted from a FOV value and the culling camera's
// aspect. With the flat values the cascades only covered the middle of the VR view, so shadows vanished at the edges.
// The cascades are fitted to slices of the view frustum by depth (distance along the view direction), so a far point
// at the side of the wide VR view is still inside the last cascade but drops out of it once it is looked at directly:
// distant shadows (cliffs, rocks) came and went while turning the head. The last cascade's range is stretched by
// ShadowFarScale (about 1 / cos of the half view angle) so it reaches as far straight ahead as it does at the edges.
static bool g_shadowFarOn = true;   // F2 toggles (comparison)
typedef void (__cdecl *CascadeFit_t)(float*, float*, int, int, float*, float*, int, void*);
static void __cdecl Hook_CascadeFit(float* a1, float* a2, int a3, int cam, float* fov, float* ranges, int a7, void* a8) {
    if (!g_reflWide || g_reflFov <= 0 || !cam) { ((CascadeFit_t)A(0x103AD0D0))(a1, a2, a3, cam, fov, ranges, a7, a8); return; }
    static int logged = 0;
    float r[4] = { 0, 0, 0, 0 }; float* useR = ranges;
    if (ranges && !IsBadReadPtr(ranges, 12)) {
        r[0] = ranges[0]; r[1] = ranges[1]; r[2] = ranges[2];
        if (g_cfg.shadowFarScale > 1.0f && g_shadowFarOn) { r[2] *= g_cfg.shadowFarScale; useR = r; }
    }
    if (logged < 2) { ++logged; LOG("sun shadow cascades: fov %.3f aspect %.3f -> %.3f %.3f, ranges %.1f %.1f %.1f -> last %.1f", *fov, *(float*)(cam + 0x18), g_reflFov, g_reflAsp, ranges ? ranges[0] : 0, ranges ? ranges[1] : 0, ranges ? ranges[2] : 0, r[2]); }
    float asp = *(float*)(cam + 0x18); *(float*)(cam + 0x18) = g_reflAsp;
    float wf = g_reflFov;
    // With radial cascades the game's own spheres still decide which shadow casters are drawn (the light-space
    // culling volume it builds from them). Those spheres lie ahead of the eye, so casters at the edges of the view
    // (a pebble's or a bush's shadow on a rock) dropped out there. The fit is run with the ranges stretched so its
    // spheres reach around the radial ones across the whole view.
    const bool radial = g_cfg.shadowRadial > 0 && g_shadowFarOn && useR;
    float fitR[4] = { 0, 0, 0, 0 }; float* fitUse = useR;
    if (radial) { fitR[0] = useR[0] * g_cfg.shadowRadial; fitR[1] = useR[1] * g_cfg.shadowRadial; fitR[2] = useR[2]; fitUse = fitR; }
    ((CascadeFit_t)A(0x103AD0D0))(a1, a2, a3, cam, &wf, fitUse, a7, a8);
    *(float*)(cam + 0x18) = asp;
    // Radial cascades. The game fits each cascade to a sphere around a slice of the view frustum (entries of
    // center xyz + radius; the center lies on the view axis at depth radius + near). With the very wide VR view those
    // spheres sit far ahead, so which cascade covers a spot (and which small casters - bushes, grass - leave a shadow
    // there) changed as the head turned, with a hard edge. Centering every sphere on the eye with radius = range x
    // ShadowRadialScale makes the coverage the same in every direction.
    if (radial && a3) {
        float* sp = (float*)a3; const float n = *(float*)(cam + 0x220);
        const float c0 = sp[3] + n, c1 = sp[7] + n;
        if (c1 - c0 > 1e-3f && std::isfinite(c0) && std::isfinite(c1)) {
            float fw[3], P[3];
            for (int k = 0; k < 3; ++k) { fw[k] = (sp[4 + k] - sp[k]) / (c1 - c0); P[k] = sp[k] - fw[k] * c0; }
            const float fl = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
            if (fl > 0.9f && fl < 1.1f && std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2])) {
                static int lg = 0;
                if (lg < 2) { ++lg; LOG("sun shadow cascades: radii %.1f %.1f %.1f (centers %.1f %.1f %.1f m ahead) -> radial %.1f %.1f %.1f around the eye", sp[3], sp[7], sp[11], c0, c1, sp[11] + n, useR[0] * g_cfg.shadowRadial, useR[1] * g_cfg.shadowRadial, useR[2] * (g_cfg.shadowRadial > 1.0f ? 1.0f : g_cfg.shadowRadial)); }
                // a1 = how far the sphere center moves (in light space) per meter of radius; the shaders pick a pixel's
                // cascade from it (spheres touching the eye and reaching forward). With all spheres on the eye it is 0,
                // so the cascade is picked by distance from the eye, matching the radial maps. a2 (the eye on the light
                // plane) stays as the game computed it.
                if (a1 && !IsBadWritePtr(a1, 8)) {
                    static int la = 0; if (la < 2) { ++la; LOG("sun shadow cascades: cascade pick %.3f %.3f -> 0 0 (radial)", a1[0], a1[1]); }
                    a1[0] = 0.0f; a1[1] = 0.0f;
                }
                for (int i = 0; i < 3; ++i) {
                    sp[i * 4 + 0] = P[0]; sp[i * 4 + 1] = P[1]; sp[i * 4 + 2] = P[2];
                    sp[i * 4 + 3] = useR[i] * (i < 2 ? g_cfg.shadowRadial : (g_cfg.shadowRadial > 1.0f ? 1.0f : g_cfg.shadowRadial));
                }
            }
        }
    }
}
static bool PatchCall(uintptr_t va, uintptr_t callee, void* fn, const char* what) {   // va: a "call callee" (Steam VAs)
    uint8_t* p = (uint8_t*)A(va); uintptr_t want = A(callee);
    if (!p || !want || p[0] != 0xE8 || (uintptr_t)p + 5 + *(int32_t*)(p + 1) != want) { LOG("%s: unexpected bytes, not applied", what); return false; }
    DWORD old; VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)fn - ((uintptr_t)p + 5));
    VirtualProtect(p, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), p, 5);
    LOG("%s installed", what);
    return true;
}
// Shadow resolution. The sun's three cascades are packed side by side in one shadow map of
// CascadedShadowMapSize*3 x CascadedShadowMapSize (quality config +0x7C8, 2048 at Ultra High). The shadow renderer
// reserves that map when it is created (0x103AEC00), so the size is raised there. D3D10 limits textures to 8192
// texels, so the largest cascade is 2730.
typedef void (__fastcall *ShadowInit_t)(uint32_t* self);
static ShadowInit_t o_ShadowInit = nullptr; static uint8_t* g_shadowCfg = nullptr;
static void __fastcall Hook_ShadowInit(uint32_t* self) {
    uint8_t* cfg = self ? (uint8_t*)(uintptr_t)self[3] : nullptr;
    if (cfg && g_cfg.shadowMapSize > 0) {
        int want = g_cfg.shadowMapSize, cur = *(int*)(cfg + 0x7C8);
        if (want > cur) { *(int*)(cfg + 0x7C8) = want; g_shadowCfg = cfg; }
        LOG("shadow map: cascade size %d -> %d (map %dx%d)", cur, *(int*)(cfg + 0x7C8), *(int*)(cfg + 0x7C8) * 3, *(int*)(cfg + 0x7C8));
    }
    o_ShadowInit(self);
}
static void KeepShadowSize() {   // the quality presets may write the value again later; keep it matching the map
    if (g_shadowCfg && *(int*)(g_shadowCfg + 0x7C8) != g_cfg.shadowMapSize) {
        { static int nl = 0; if (nl++ < 20) LOG("shadow map: cascade size reset to %d by the game, restoring %d", *(int*)(g_shadowCfg + 0x7C8), g_cfg.shadowMapSize); }
        *(int*)(g_shadowCfg + 0x7C8) = g_cfg.shadowMapSize;
    }
}
// ---- Shared sun shadow maps. Both eyes' scenes are rendered from the same head-centred culling camera, so the sun's
// cascades (one atlas texture, three passes) come out identical for both eyes; culling and gathering their casters is
// over half of the main thread's time in a busy town. With ShareShadows the right eye, for the sun only:
//  - skips the casters' culling in the shadow renderer's setup (0x103AE000 -> culler setup 0x103766A0 at 0x103AEB82,
//    traversal 0x10375260 at 0x103AEB93; the light being set up is the caller's [esp+2Ch], type +4: 0 = sun),
//  - skips gathering casters into the sun's passes (0x103A9140, thiscall(view, passId)),
//  - and when the passes are executed (0x103A9950 -> 0x1039D900 per pass) its sun passes only reset (no clear, no
//    draw), so it samples the atlas the left eye drew. Other lights (fires, explosions) cast per eye as before: their
//    maps are sized and placed from each eye's own view. The sun atlas is the texture several left-eye passes share.
static bool Readable(const void* p, size_t n);
typedef void (__fastcall *ShGather_t)(void* self, void* edx, void* view, uint32_t pass);
typedef void (__fastcall *ShSimple_t)(void* self);
static ShGather_t o_ShGather = nullptr; static ShSimple_t o_ShDraw = nullptr, o_PassExec = nullptr;
static ID3D10Texture2D* ShTex(void* obj);   // below (needs the device)
static ID3D10Texture2D* g_shLeftTex[64]; static uint32_t g_shLeftId[64]; static int g_shLeftN = 0; static bool g_shLeftDone = false;
static uint8_t* g_shKept[64]; static int g_shKeptN = 0;   // right-eye sun pass objects this scene
static ID3D10Texture2D* g_shAtlas = nullptr;   // the left eye's sun atlas (null: nothing to share this frame)
static bool g_shInDraw = false, g_shSkipCull = false;
static volatile LONG g_shShared = 0, g_shRendered = 0, g_shPassReset = 0, g_shCullSkipped = 0, g_shGatherSkipped = 0, g_shOddAtlas = 0;
static ID3D10Texture2D* ShPassDepth(void* self, uint32_t id) {
    uint8_t* mgr = *(uint8_t**)((uint8_t*)self + 0x18); if (!mgr) return nullptr;
    uint8_t** passes = *(uint8_t***)(mgr + 4); if (!passes || id >= 0x4000) return nullptr;
    uint8_t* p = passes[id]; if (!p) return nullptr;
    void* o = *(void**)(p + 0x38); return o ? ShTex(o) : nullptr;
}
static bool ShRightShares() { return g_renderEye == 1 && g_cfg.shareShadows && g_shLeftDone && g_shAtlas; }
static void __fastcall Hook_ShGather(void* self, void* edx, void* view, uint32_t pass) {
    ID3D10Texture2D* t = ShPassDepth(self, pass);
    if (g_renderEye == 0) {
        if (g_shLeftDone) { g_shLeftDone = false; g_shLeftN = 0; g_shAtlas = nullptr; }   // a new left eye: record afresh
        if (g_shLeftN < 64) { g_shLeftId[g_shLeftN] = pass; g_shLeftTex[g_shLeftN++] = t; }
    } else if (ShRightShares() && [&] { for (int i = 0; i < g_shLeftN; ++i) if (g_shLeftId[i] == pass && g_shLeftTex[i] == g_shAtlas) return true; return false; }()) {
        // nothing is gathered, but the pass must look non-empty: for a light whose passes are all empty the shadow
        // pass execution (0x103A9950) clears the map and turns that light's shadow off for this scene. The count is
        // put back to 0 before the pass executes (Hook_PassExec), so it only resets.
        uint8_t* p = (*(uint8_t***)(*(uint8_t**)((uint8_t*)self + 0x18) + 4))[pass];
        if (p && *(uint8_t**)p && *(int*)(*(uint8_t**)p + 4) == 0) *(int*)(*(uint8_t**)p + 4) = 1;
        if (p && g_shKeptN < 64) g_shKept[g_shKeptN++] = p;
        uint8_t* p1 = (*(uint8_t***)(*(uint8_t**)((uint8_t*)self + 0x18) + 4))[pass | 1];   // its 0x1C1 companion pass
        if (p1 && g_shKeptN < 64) g_shKept[g_shKeptN++] = p1;
        InterlockedIncrement(&g_shGatherSkipped); return;
    }
    o_ShGather(self, edx, view, pass);
}
static void __fastcall Hook_ShDraw(void* self) {   // executes the shadow passes, once per scene
    if (g_renderEye == 0) {
        o_ShDraw(self); InterlockedIncrement(&g_shRendered); g_shLeftDone = true;
        g_shAtlas = nullptr;   // the sun atlas: a texture used by more than one pass
        for (int i = 0; i < g_shLeftN && !g_shAtlas; ++i) for (int j = i + 1; j < g_shLeftN; ++j) if (g_shLeftTex[i] && g_shLeftTex[i] == g_shLeftTex[j]) { g_shAtlas = g_shLeftTex[i]; break; }
        return;
    }
    bool share = ShRightShares();
    if (share) InterlockedIncrement(&g_shShared);
    if (g_renderEye == 1) {
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 10000) { last = now; LOG("shared sun shadows: %ld of %ld right eyes shared (culls skipped %ld, gathers skipped %ld, passes kept %ld, other atlas %ld; %d passes)",
            InterlockedExchange(&g_shShared, 0), InterlockedExchange(&g_shRendered, 0), InterlockedExchange(&g_shCullSkipped, 0), InterlockedExchange(&g_shGatherSkipped, 0), InterlockedExchange(&g_shPassReset, 0), InterlockedExchange(&g_shOddAtlas, 0), g_shLeftN); }
    }
    g_shInDraw = share; o_ShDraw(self); g_shInDraw = false; g_shKeptN = 0;
}
static void __fastcall Hook_PassExec(void* pass) {   // 0x1039D900: execute (clear + draw) and reset a render pass
    if (g_shInDraw) {
        bool kept = false; for (int i = 0; i < g_shKeptN; ++i) kept |= g_shKept[i] == (uint8_t*)pass;
        void* o = *(void**)((uint8_t*)pass + 0x38);
        ID3D10Texture2D* t = o ? ShTex(o) : nullptr;
        if (kept || (t && t == g_shAtlas)) {   // a right-eye sun pass: nothing gathered; no clears, so it only resets
            uint8_t* p = (uint8_t*)pass;
            *(int*)(p + 4) = 0; *(int*)(p + 0x2C) = 0; *(int*)(p + 0x30) = 0; p[0x74] = 0;
            if (*(uint8_t**)p) *(int*)(*(uint8_t**)p + 4) = 0;   // the count Hook_ShGather made look non-empty
            InterlockedIncrement(&g_shPassReset);
        } else if (t) { D3D10_TEXTURE2D_DESC a{}, b{}; t->GetDesc(&a); g_shAtlas->GetDesc(&b); if (a.Width == b.Width && a.Height == b.Height) InterlockedIncrement(&g_shOddAtlas); }
    }
    o_PassExec(pass);
}
// the casters' culling: skipped for the sun in the right eye (the caller's light at its [esp+2Ch], 7 args pushed)
typedef void (__fastcall *CullSetup_t)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g);
typedef void (__fastcall *CullRun_t)(void* self, void* edx, uint32_t a, uint32_t b);
static void __fastcall ShCullSetup(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    g_shSkipCull = false;
    if (ShRightShares()) {
        uint8_t* args = (uint8_t*)__builtin_frame_address(0) + 8;   // our first stack argument = the caller's esp
        uint8_t* light = *(uint8_t**)(args + 0x1C + 0x2C);
        if (light && Readable(light + 4, 4) && *(int*)(light + 4) == 0) { g_shSkipCull = true; InterlockedIncrement(&g_shCullSkipped); return; }
    }
    ((CullSetup_t)A(0x103766A0))(self, edx, a, b, c, d, e, f, g);
}
static void __fastcall ShCullRun(void* self, void* edx, uint32_t a, uint32_t b) {
    if (g_shSkipCull) { g_shSkipCull = false; return; }
    ((CullRun_t)A(0x10375260))(self, edx, a, b);
}
static void InstallShadowShare() {
    static const uint8_t proG[8] = { 0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x1C };   // sub esp,10h; push ebx; mov ebx,[esp+1Ch]
    static const uint8_t pro[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };                 // push ebp; mov ebp,esp; and esp,-16
    static const uint8_t proX[6] = { 0x83, 0xEC, 0x18, 0x53, 0x33, 0xDB };                // sub esp,18h; push ebx; xor ebx,ebx
    bool a = PatchJmpN(A(0x103A9140), proG, 8, (void*)&Hook_ShGather, (void**)&o_ShGather);
    bool b = a && PatchJmpN(A(0x103A9950), pro, 6, (void*)&Hook_ShDraw, (void**)&o_ShDraw);
    bool x = b && PatchJmpN(A(0x1039D900), proX, 6, (void*)&Hook_PassExec, (void**)&o_PassExec);
    bool c = x && PatchCall(0x103AEB82, 0x103766A0, (void*)&ShCullSetup, "shared shadow maps: caster culling setup");
    bool d = c && PatchCall(0x103AEB93, 0x10375260, (void*)&ShCullRun, "shared shadow maps: caster culling run");
    LOG("shared sun shadows: hooks %d %d %d %d %d (%s)", a, b, x, c, d, g_cfg.shareShadows ? "on" : "off");
}
static void InstallReflFix() {
    PatchCall(0x103A8B9D, 0x1039FEA0, (void*)&Hook_ReflCamCopy, "water reflection fix");      // call camera copy (water reflection)
    if (g_cfg.shadowFix) {
        PatchCall(0x103AE098, 0x1039FEA0, (void*)&Hook_ReflCamCopy, "shadow caster camera fix"); // call camera copy (sun shadows)
        PatchCall(0x103AE5BC, 0x103AD0D0, (void*)&Hook_CascadeFit, "shadow cascade fix");        // call cascade fit
    }
    if (g_cfg.shadowMapSize > 0) {
        static const uint8_t pro[5] = { 0x83, 0xEC, 0x14, 0x53, 0x56 };   // sub esp,14h; push ebx; push esi
        LOG("shadow map size hook %d", PatchJmpN(A(0x103AEC00), pro, 5, (void*)&Hook_ShadowInit, (void**)&o_ShadowInit));
    }
}
static void InstallAmmoHook();
// The game's bullet counter (HUD): CBulletCounter vtable slot 6, 0x1086A1F0 (thiscall, clip, pouch; ret 8) sets the
// "AmmoInClipText" / "AmmoInPouchText" numbers. Its values are drawn by the mod in the scope view.
typedef void (__fastcall *SetAmmo_t)(void* self, void* edx, uint32_t clip, uint32_t pouch);
static SetAmmo_t o_SetAmmo = nullptr;
static volatile LONG g_hudClip = -1, g_hudPouch = -1, g_hudAmmoSeq = 0;
static void __fastcall Hook_SetAmmo(void* self, void* edx, uint32_t clip, uint32_t pouch) {
    if (clip != (uint32_t)g_hudClip || pouch != (uint32_t)g_hudPouch) { g_hudClip = (LONG)clip; g_hudPouch = (LONG)pouch; InterlockedIncrement(&g_hudAmmoSeq); arms::NotifyClip((int)clip, (int)pouch); }
    o_SetAmmo(self, edx, clip, pouch);
}
static void InstallAmmoHook() {
    static const uint8_t pro[5] = { 0x83, 0xEC, 0x08, 0x33, 0xC0 };   // sub esp,8; xor eax,eax
    LOG("bullet counter hook %d", PatchJmpN(A(0x1086A1F0), pro, 5, (void*)&Hook_SetAmmo, (void**)&o_SetAmmo));
}
// ---- HUD elements. The main HUD's setup (0x1080BB30, thiscall(hud, root), ret 4) hands us the root UI element.
// Element: id hash +8 (game hash 0x10AA7150 of the name), children vector +0x28/+0x2C, flags +0x48 (bit 0 = visible).
// The interaction prompts are hidden for good; the reticle group "a_crosshair" is hidden too, but its per-weapon
// children ("a_ak47", "a_m249", ...) still tell us when the game would show a reticle (the dot replaces it).
typedef void (__fastcall *HudSetup_t)(void* hud, void* edx, void* root);
static HudSetup_t o_HudSetup = nullptr;
static uint8_t* volatile g_hudRoot = nullptr; static uint8_t* volatile g_hudObj = nullptr;
static void __fastcall Hook_HudSetup(void* hud, void* edx, void* root) {
    o_HudSetup(hud, edx, root);
    g_hudObj = (uint8_t*)hud; g_hudRoot = (uint8_t*)root; LOG("HUD %p root element %p page root %p", hud, root, *(void**)((uint8_t*)hud + 0x11C));
}
static uint32_t UiHash(const char* name) { typedef void (__cdecl *H_t)(uint32_t* out, const char* s); uint32_t h = 0; ((H_t)A(0x10AA7150))(&h, name); return h; }
static bool UiChildren(uint8_t* e, uint8_t**& b, uint8_t**& en) {
    if (!e || IsBadReadPtr(e, 0x4C)) return false;
    b = *(uint8_t***)(e + 0x28); en = *(uint8_t***)(e + 0x2C);
    if (!b || en < b || en - b > 4096 || IsBadReadPtr(b, (en - b) * 4)) return false;
    return true;
}
static uint8_t* UiFind(uint8_t* e, uint32_t h, int depth) {
    if (!e || depth > 10 || IsBadReadPtr(e, 0x4C)) return nullptr;
    if (*(uint32_t*)(e + 8) == h) return e;
    uint8_t **b, **en; if (!UiChildren(e, b, en)) return nullptr;
    for (uint8_t** c = b; c < en; ++c) if (uint8_t* r = UiFind(*c, h, depth + 1)) return r;
    return nullptr;
}
static volatile bool g_gameReticle = false; static volatile bool g_hudOk = false;
static void HudTick() {   // every frame
    static uint8_t* root = nullptr; static uint8_t* hide[24] = {}; static int nHide = 0; static uint8_t* cross = nullptr;
    static double nextScan = 0; static int scans = 0; static bool found[24] = {};
    static const char* names[] = { "InteractionSwitchGroup", "CurrentInteractionIcon", "TargetInteractionIcon", "CurrentInteractionIconFade", "TargetInteractionIconFade",
                                   "interact", "hudprompt", "Hit_Borders", "HitIndicator_0", "HitIndicator_1", "HitIndicator_2", "HitIndicator_3",
                                   "HitIndicator_4", "HitIndicator_5", "HitIndicator_6", "HitIndicator_7" };
    const int nNames = (int)(sizeof names / sizeof names[0]);
    uint8_t* r = g_hudRoot; uint8_t* hud = g_hudObj;
    if (!r || !hud) { g_hudOk = false; return; }
    if (r != root) { root = r; nHide = 0; cross = nullptr; nextScan = 0; scans = 0; memset(found, 0, sizeof found); }
    double now = NowMs();
    if (now >= nextScan && scans < 30) {   // elements can be added after the HUD setup: look again for a while
        ++scans; nextScan = now + 2000.0;
        for (int i = 0; i < nNames; ++i) {
            if (found[i]) continue;
            bool hurt = i >= 7;
            if (hurt ? !g_cfg.hideHurt : !g_cfg.hidePrompts) continue;
            uint8_t* e = UiFind(root, UiHash(names[i]), 0);
            if (e && nHide < 24) { hide[nHide++] = e; found[i] = true; LOG("HUD: hiding %s (%p)", names[i], (void*)e); }
        }
        if (!cross) cross = UiFind(root, UiHash("a_crosshair"), 0);
        if (scans == 1 || scans == 30) LOG("HUD: scan %d: %d elements hidden, reticle group %p", scans, nHide, (void*)cross);
        if (scans == 15) {   // the element tree (name hashes), to find what to hide
            struct D { static void Dump(uint8_t* e, int depth, int& cnt) {
                if (!e || depth > 8 || cnt > 600 || IsBadReadPtr(e, 0x4C)) return;
                uint8_t **b, **en; bool ch = UiChildren(e, b, en);
                char pad[20]; int d = depth < 18 ? depth : 18; memset(pad, ' ', d); pad[d] = 0;
                LOG("HUD tree %s%08x vis %d kids %d (%p)", pad, *(uint32_t*)(e + 8), e[0x48] & 1, ch ? (int)(en - b) : -1, (void*)e); ++cnt;
                if (ch) for (uint8_t** c = b; c < en; ++c) Dump(*c, depth + 1, cnt);
            } };
            int cnt = 0; D::Dump(root, 0, cnt);
        }
    }
    for (int i = 0; i < nHide; ++i) if (!IsBadReadPtr(hide[i], 0x4C)) hide[i][0x48] &= ~1;
    // the HUD's reticle update (0x1080AF10) keeps the current weapon's reticle at +0x188 (0 = this weapon has none)
    bool req = !IsBadReadPtr(hud + 0x188, 4) && *(uint32_t*)(hud + 0x188) != 0;
    static bool lastReq = false; if (req != lastReq) { lastReq = req; LOG("HUD: game reticle %s", req ? "on" : "off"); }
    if (cross && !IsBadReadPtr(cross, 0x4C) && g_cfg.reticleDot) cross[0x48] &= ~1;   // the dot replaces every game reticle
    g_gameReticle = req; g_reticleOnUi = req; g_hudOk = true;
    {   // the current weapon's type for the saved weapon holds: the reticle element's name id
        uint32_t v = req ? *(uint32_t*)(hud + 0x188) : 0, key = 0;
        if (v && !IsBadReadPtr((void*)(uintptr_t)v, 0x0C)) key = *(uint32_t*)((uint8_t*)(uintptr_t)v + 8); else key = v;
        static uint32_t lastKey = 1; if (key != lastKey) { lastKey = key; LOG("HUD: weapon type %08x (reticle %08x)", key, v); }
        arms::SetWeaponKey(key);
    }
    UiCensusTick();
    // top-level HUD pages: log when one shows or hides
    { uint8_t **b, **en; static uint8_t lastVis[64]; static bool init = false;
      if (UiChildren(root, b, en) && en - b <= 64) { for (int i = 0; i < en - b; ++i) { uint8_t* e = b[i]; if (!e || IsBadReadPtr(e, 0x4C)) continue;
          uint8_t v = e[0x48] & 1; if (init && v != lastVis[i]) LOG("HUD page %08x %s", *(uint32_t*)(e + 8), v ? "shown" : "hidden"); lastVis[i] = v; } init = true; } }
}
static void InstallHudHook() {
    static const uint8_t pro[5] = { 0x83, 0xEC, 0x7C, 0x53, 0x55 };   // sub esp,7Ch; push ebx; push ebp
    LOG("HUD setup hook %d", PatchJmpN(A(0x1080BB30), pro, 5, (void*)&Hook_HudSetup, (void**)&o_HudSetup));
}
static void InstallNpcFix() {
    if (g_cfg.fixBouncingNpcs) {
        uint8_t* p = (uint8_t*)A(0x10493CB2);
        static const uint8_t expect[7] = { 0x80, 0xBB, 0xC2, 0x00, 0x00, 0x00, 0x00 };   // cmp byte [ebx+0C2h],0
        if (memcmp(p, expect, 7) == 0) {
            DWORD old; VirtualProtect(p + 2, 1, PAGE_EXECUTE_READWRITE, &old); p[2] = 0xC0; VirtualProtect(p + 2, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 7);
            LOG("NPC fix: rigid-body character controller only for archetypes with bUpdateRotation (animals)");
        } else LOG("NPC fix: unexpected bytes at 0x10493CB2, not applied");
    }
    if (g_cfg.npcFixThreads) {
        static const uint8_t pro[5] = { 0x83, 0xEC, 0x24, 0x53, 0x56 };   // sub esp,24h; push ebx; push esi
        bool ok = PatchJmpN(A(0x102B3E80), pro, 5, (void*)&Hook_IdealThreads, (void**)&o_IdealThreads);
        LOG("NPC fix: thread-count hook %d", ok);
    }
}

// LOD / draw-distance boost. The renderer's config object (this+0x2C in RenderFrame) holds the "Geometry"
// quality section at +0x660 (LodScale, KillLodScale, ... registered in 0x103F5xxx).
// LodLevel (ini, 1..8) scales every distance-type field up and every minimum-size field down by the same factor. The RealTree*MinSize fields hide tree parts that look small on screen: distant trunks
// (nodes) vanished while their leaves stayed.
struct LodField { int off; const char* name; int kind; float orig, written; bool have; };   // kind 0 = distance (x level), 1 = min size (/ level)
int g_noAntiPortals = 1, g_noVisTest = 1; float g_lodLevelIni = 2.0f; static float g_lodLevel = -1.0f;
static LodField g_lod[] = {
    { 0x6b4, "LodScale", 0 },
    { 0x6b8, "KillLodScale", 0 },
    { 0x6b0, "ClustersLodScale", 0 },
    { 0x6ac, "RealTreesLodScale", 0 },
    { 0x6a8, "RealTreeCapsMaxDistance", 0 },
    { 0x6c0, "DetailDistanceScaleModifier", 0 },
    { 0x6ec, "SceneObjectMinSize", 1 },
    { 0x6f0, "SceneObjectDepthPassMinSize", 1 },
    { 0x6e4, "ClusterObjectMinSize", 1 },
    { 0x6d4, "RealTreeLeafMinSize", 1 },
    { 0x6d8, "RealTreeHLeafMinSize", 1 },
    { 0x6dc, "RealTreeNodeMinSize", 1 },
};
bool g_lodOn = false; static volatile double g_macheteCalAt = 0; static XrPosef g_menuPoseX{};   // set from LodBoost at startup
static void ShowToast(const char* t);
static void ApplyLod(void* self) {
    KeepShadowSize();
    { static bool prev = false; bool k = (DevKey(VK_F2) & 0x8000) != 0; bool sh = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ct = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      if (k && !prev && (sh || ct)) {   // Shift+F2 / Ctrl+F2: sharp-shadow reach up / down (live; ini ShadowRadialScale)
          g_cfg.shadowRadial += sh ? 0.5f : -0.5f; if (g_cfg.shadowRadial < 1.0f) g_cfg.shadowRadial = 1.0f; if (g_cfg.shadowRadial > 10.0f) g_cfg.shadowRadial = 10.0f;
          { float farR = 240.0f * g_cfg.shadowFarScale, medR = 60.0f * g_cfg.shadowRadial; if (medR > farR * 0.8f) { g_cfg.shadowFarScale = medR / 0.8f / 240.0f; if (g_cfg.shadowFarScale > 5.0f) g_cfg.shadowFarScale = 5.0f; } }
          LOG("sun shadows: radial x%.1f -> sharp shadows reach %.0f m, medium %.0f m", g_cfg.shadowRadial, 20.0f * g_cfg.shadowRadial, 60.0f * g_cfg.shadowRadial);
          prev = k; } else
      if (k && !prev) { g_shadowFarOn = !g_shadowFarOn; LOG("sun shadows: VR cascade fix (far x%.2f, radial x%.2f) %s", g_cfg.shadowFarScale, g_cfg.shadowRadial, g_shadowFarOn ? "on" : "off"); } prev = k; }
    {   // occlusion culling tested from the flat culling camera: anti-portals (occluder volumes inside rocks / hills) and
        // the visibility tests (occlusion queries) can hide what the eyes do see. CRenderConfig (*0x11609560) +0x70
        // DisableAntiPortals, +0x84 DisableVisibilityTest (ini DisableAntiPortals / DisableVisibilityTest).
        extern int g_noAntiPortals, g_noVisTest;
        {   // Home: cycle the two occlusion settings live (performance A/B tests); Pause: a marker line in the log
            // v202: the Home key (shared shadows on/off) removed - set in the launcher
            static bool prevP = false; bool p = (DevKey(VK_PAUSE) & 0x8000) != 0;
            if (p && !prevP) { static int n = 0; ++n; LOG("===== MARK %d (Pause key) =====", n); char t[48]; snprintf(t, sizeof t, "Log mark %d", n); ShowToast(t); }
            prevP = p;
        }
        uint8_t* rc = *(uint8_t**)A(0x11609560);
        if (rc && !IsBadWritePtr(rc + 0x70, 0x18)) {
            static int last = -1; uint8_t a = g_noAntiPortals ? 1 : 0, v = g_noVisTest ? 1 : 0;
            if (rc[0x70] != a) rc[0x70] = a;
            if (rc[0x84] != v) rc[0x84] = v;
            if (last != a * 2 + v) { last = a * 2 + v; LOG("render config %p: DisableAntiPortals = %d, DisableVisibilityTest = %d", (void*)rc, a, v); }
        }
    }
    if (g_lodLevel < 0) { g_lodLevel = g_lodLevelIni; if (g_lodLevel < 1.0f) g_lodLevel = 1.0f; if (g_lodLevel > 8.0f) g_lodLevel = 8.0f; }
    if (!self || g_lodLevel <= 1.0f) return;   // v199: LOD left to the game
    uint8_t* cfg = *(uint8_t**)((uint8_t*)self + 0x2C);
    if (!cfg || IsBadReadPtr(cfg + 0x660, 0xA0)) return;
    static bool logged = false, bad = false;
    if (bad) return;
    { float ls = *(float*)(cfg + 0x6b4), ks = *(float*)(cfg + 0x6b8);
      if (!g_lod[0].have && !(ls > 0.01f && ls < 100.f && ks > 0.01f && ks < 100.f)) { bad = true; LogWrite("LOD config at %p looks wrong (LodScale %g, KillLodScale %g); LOD boost disabled", (void*)cfg, ls, ks); return; } }
    // Loading with the boost already on left many objects, buildings and trees missing until the level was stepped
    // down to x1 (logs: Ctrl+F4 pressed down to x1.0 after every load). So the world loads at the game's own values
    // (x1) and the boost is brought in afterwards, in steps of 0.5 (as the key presses did): x1 until 2 s after the
    // controls leave a loading screen (or the game resets the values), then +0.5 every 0.3 s.
    static double rampBase = -1; static bool wasNormal = true; static double notNormalSince = 0; static float lastLevel = -1;
    double now = NowMs();
    { bool ld = controls::LoadingControls();   // "wasNormal" = not on the loading-screen controls
      if (ld && wasNormal) notNormalSince = now;
      if (!ld && !wasNormal && notNormalSince > 0 && now - notNormalSince > 1500.0) rampBase = now;
      if (ld && notNormalSince > 0 && now - notNormalSince > 1500.0) rampBase = -2;   // a loading screen: hold at x1
      wasNormal = !ld; }
    for (auto& f : g_lod) { float v = *(float*)(cfg + f.off); if (!f.have || v != f.written) { rampBase = now; break; } }
    float level = g_lodLevel;
    if (rampBase == -2) level = 1.0f;
    else if (rampBase >= 0) {
        double t = now - rampBase - 2000.0;
        level = t < 0 ? 1.0f : fminf(g_lodLevel, 1.0f + 0.5f * (float)(1 + (int)(t / 300.0)));
        if (level >= g_lodLevel) rampBase = -1;
    }
    if (level != lastLevel) { static int nl = 0; if (nl++ < 60) LOG("LOD: level x%.1f%s", level, level >= g_lodLevel ? " (full)" : ""); lastLevel = level; }
    for (auto& f : g_lod) {
        float& v = *(float*)(cfg + f.off);
        if (!f.have || v != f.written) { f.orig = v; f.have = true; }   // first time, or the game reloaded its settings
        float want = !g_lodOn ? f.orig : f.kind == 0 ? f.orig * level : f.orig / level;
        v = want; f.written = want;
    }

    if (!logged) { logged = true; char b[512]; int o = snprintf(b, sizeof b, "LOD config at %p:", (void*)cfg);
        for (auto& f : g_lod) o += snprintf(b + o, sizeof b - o, " %s=%g", f.name, f.orig); LogWrite("%s (level %g, %s)", b, g_lodLevel, g_lodOn ? "on" : "off"); }
}
static void __fastcall Hook_RenderFrame(void* self) {
    ApplyLod(self);
    g_tRFStart = NowMs();
    if (g_tPresentEnd > 0) g_sec.sim += g_tRFStart - g_tPresentEnd;
    g_tHookStart = g_tHookEnd = 0;
    perf::SetPhase(perf::P_RFPRE);
    Trace("RF>");
    if (g_capState == 2) TraceMark("RenderFrame begin");
    o_RenderFrame(self);
    if (g_capState == 2) TraceMark("RenderFrame end");
    Trace("RF<");
    perf::SetPhase(perf::P_MID);
    g_tRFEnd = NowMs();
    g_rfMs += g_tRFEnd - g_tRFStart;
    if (g_tHookStart > 0) { g_sec.rfPre += g_tHookStart - g_tRFStart; g_sec.hook += g_tHookEnd - g_tHookStart; g_sec.rfPost += g_tRFEnd - g_tHookEnd; }
}
static RenderViewport_t o_RenderViewport = nullptr;
static Present_t        o_Present = nullptr;

static bool g_stereoOn = true;
static float g_detailT = 0; static double* g_detailLastWaitPtr = nullptr;   // adaptive detail (0 = flat FOV for LOD)
static int g_cullMode = 1;   // F6: 0 = wide VR culling camera, 1 = wide planes + flat FOV/projection, 2 = game's own culling camera
static bool g_xrFailed = false;
static bool g_eyesThisFrame = false;
static bool g_vpThisPresent = false;
static int g_fovMode = 2;   // 0 = headset's own frustums, 1 = symmetric, 2 = symmetric + hidden overscan border (F3 cycles)
static bool g_vpEyes = false, g_screenFrame = false; static xr::FrameViews g_screenFv;
// Frame-path recorder: one letter per presented frame, logged as runs when the pattern changes.
// E = stereo eyes, Z = scope screen, Q = flat screen inside a VP frame, S = flat screen (menu/loading), x = backbuffer
// size mismatch (nothing submitted), - = no backbuffer.
static char g_pathLast = 0; static int g_pathRun = 0; static char g_pathLine[256]; static int g_pathLen = 0; static DWORD g_pathT = 0;
static void PathFlush() { if (g_pathLen) { LogWrite("frames: %.*s", g_pathLen, g_pathLine); g_pathLen = 0; } }
static void PathNote(char c) {
    if (g_pathLen && GetTickCount() - g_pathT > 1000) { g_pathT = GetTickCount(); if (g_pathLast) g_pathLen += snprintf(g_pathLine + g_pathLen, sizeof g_pathLine - g_pathLen, "(%c%d...) ", g_pathLast, g_pathRun); PathFlush(); }
    if (c == g_pathLast) { ++g_pathRun; return; }
    if (g_pathLast) { g_pathLen += snprintf(g_pathLine + g_pathLen, sizeof g_pathLine - g_pathLen, "%c%d ", g_pathLast, g_pathRun); }
    bool interesting = g_pathLast && (g_pathLast != 'E' || c != 'E');
    g_pathLast = c; g_pathRun = 1;
    if (g_pathLen > 200 || (!interesting && g_pathLen)) PathFlush();
    if (GetTickCount() - g_pathT > 2000) { g_pathT = GetTickCount(); PathFlush(); }
}
// Scope / binocular mode: while the game zooms (camera FOV well below its normal value) the frame is rendered flat,
// as the game does, and shown as a single screen in front of the player with black around it (as CrysisVR does).
static bool g_scopeOn = false, g_scopeFrame = false; static volatile bool g_twoHandGrip = false;
static float g_fovBase = 0.0f;
static XrPosef g_scopePose{};
static xr::FrameViews g_scopeFv;
static IDXGISwapChain* g_swap = nullptr;
static UINT g_xrW = 0, g_xrH = 0;      // backbuffer size the XR swapchains were created for
static bool g_haveDevice = false;

// The engine resizes the swap chain via ResizeBuffers (0x1042060C) and ignores failure, so we must
// never keep a reference to the backbuffer across frames. Fetch per use and release immediately.
static ID3D10Texture2D* AcquireBackbuffer(D3D10_TEXTURE2D_DESC* desc = nullptr) {
    uint8_t* drv = *(uint8_t**)A(eng::GraphicsDriver);
    IDXGISwapChain* sc = drv ? *(IDXGISwapChain**)(drv + 0x40) : nullptr;
    ID3D10Texture2D* bb = nullptr;
    if (!sc || FAILED(sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb))) return nullptr;
    if (desc) bb->GetDesc(desc);
    static UINT lastW = 0, lastH = 0;
    D3D10_TEXTURE2D_DESC d; bb->GetDesc(&d);
    if (d.Width != lastW || d.Height != lastH) { LogWrite("backbuffer is now %ux%u", d.Width, d.Height); lastW = d.Width; lastH = d.Height; }
    return bb;
}


// ---- off-screen eye targets (headset resolution, independent of the desktop backbuffer)
static void* g_eyeRT = nullptr;              // engine colour target object
static void* g_eyeDepth = nullptr;           // engine depth target object
static ID3D10Texture2D* g_eyeDepthTex = nullptr;   // its D3D10 texture (DLSS reads depth from it)
static ID3D10Texture2D* g_eyeLinTex = nullptr;     // the engine's linear depth target (metres), found by the census
static int g_eyePos[2] = { 0, 0 };

static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)m.BaseAddress + m.RegionSize;
}
// The D3D10 texture behind an engine render-target / view object (up to two levels down), matched by COM vtable.
static ID3D10Texture2D* ShTex(void* obj) {
    static uintptr_t vt = 0;
    if (!vt) {
        D3D10_TEXTURE2D_DESC td{}; td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D10_BIND_RENDER_TARGET;
        ID3D10Texture2D* probe = nullptr; if (!g_dev10 || FAILED(g_dev10->CreateTexture2D(&td, nullptr, &probe))) return nullptr;
        vt = *(uintptr_t*)probe; probe->Release();
    }
    auto scan = [&](uint8_t* base, int depth, auto& self) -> ID3D10Texture2D* {
        if (!Readable(base, 0x60)) return nullptr;
        for (int off = 0; off < 0x60; off += 4) { uint8_t* p = *(uint8_t**)(base + off); if (Readable(p, 4) && *(uintptr_t*)p == vt) return (ID3D10Texture2D*)p; }
        if (depth < 2) for (int off = 4; off < 0x60; off += 4) { uint8_t* p = *(uint8_t**)(base + off); if (p && p != base && Readable(p, 0x60)) if (ID3D10Texture2D* r = self(p, depth + 1, self)) return r; }
        return nullptr;
    };
    static void* ko[32]; static ID3D10Texture2D* kt[32]; static int kn = 0;   // engine objects are pooled: cache
    for (int k = 0; k < 32; ++k) if (ko[k] == obj && kt[k]) return kt[k];
    ID3D10Texture2D* t = scan((uint8_t*)obj, 0, scan);
    if (t) { ko[kn] = obj; kt[kn] = t; kn = (kn + 1) & 31; }
    return t;
}
// Find the ID3D10Texture2D held (directly or one level down) by an engine render-target object,
// by matching the COM vtable of a real D3D10 texture from the same device and the expected size.
static ID3D10Texture2D* FindTexture(void* obj, UINT w, UINT h, UINT needBind = 0) {
    D3D10_TEXTURE2D_DESC td{}; td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D10_BIND_RENDER_TARGET;
    ID3D10Texture2D* probe = nullptr;
    if (FAILED(g_dev10->CreateTexture2D(&td, nullptr, &probe))) return nullptr;
    uintptr_t vt = *(uintptr_t*)probe; probe->Release();
    auto scan = [&](uint8_t* base, int depth, auto& self) -> ID3D10Texture2D* {
        if (!Readable(base, 0x200)) return nullptr;
        for (int off = 0; off < 0x200; off += 4) {
            uint8_t* p = *(uint8_t**)(base + off);
            if (!Readable(p, 4)) continue;
            if (*(uintptr_t*)p == vt) {
                ID3D10Texture2D* t = (ID3D10Texture2D*)p; D3D10_TEXTURE2D_DESC d; t->GetDesc(&d);
                if (d.Width == w && d.Height == h && (d.BindFlags & needBind) == needBind) { LOG("eye texture found at +0x%x (depth %d) fmt=%d bind=0x%x", off, depth, (int)d.Format, d.BindFlags); return t; }
            } else if (depth == 0) {
                if (ID3D10Texture2D* r = self(p, 1, self)) return r;
            }
        }
        return nullptr;
    };
    return scan((uint8_t*)obj, 0, scan);
}
static bool CreateEyeTargets(UINT w, UINT h) {
    g_eyeRT = ((CreateColorTarget_t)A(eng::CreateColorTarget))((int)w, (int)h, 1, eng::EngineFmtLDR, 1, 0, 0, 0, 1, 2);
    g_eyeDepth = ((CreateDepthTarget_t)A(eng::CreateDepthTarget))((int)w, (int)h, 0x4B, 1, 0, 0, 0);
    LOG("engine eye targets %ux%u: colour=%p depth=%p", w, h, g_eyeRT, g_eyeDepth);
    if (!g_eyeRT || !g_eyeDepth) return false;
    g_eyeTex = FindTexture(g_eyeRT, w, h);
    if (!g_eyeTex) { LOG("could not locate the D3D10 texture inside the eye target"); return false; }
    g_eyeTex->AddRef();
    g_eyeDepthTex = FindTexture(g_eyeDepth, w, h, D3D10_BIND_DEPTH_STENCIL);
    if (g_eyeDepthTex) g_eyeDepthTex->AddRef(); else LOG("eye depth texture not found (DLSS unavailable)");
    g_eyeSize[0] = (int)w; g_eyeSize[1] = (int)h;
    return true;
}

// Enable=2: flat diagnostics. Same hooks and counters, no OpenXR, normal flat rendering.
static void FlatDiagEnsureDevice() {
    if (g_haveDevice) return;
    uint8_t* drv = *(uint8_t**)A(eng::GraphicsDriver);
    if (!drv || *(uintptr_t*)drv != A(eng::DriverVtableD3D10)) return;
    IDXGISwapChain* sc = *(IDXGISwapChain**)(drv + 0x40);
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D10Device), (void**)&g_dev10))) return;
    g_haveDevice = true;
    HookDeviceVtable(g_dev10);
    g_profArmed = true;
    LOG("flat diagnostics mode: device hooked");
}
static ID3D10RenderTargetView* g_hudRTV = nullptr;
static ID3D10Texture2D* g_preUi = nullptr;        // copy of the finished 3D eye image before the UI is drawn
static ID3D10RenderTargetView* g_eyeRTV = nullptr; // our view of the eye texture (for clearing)
static void InstallUiPatches();
static void ShowToast(const char* t);
static bool EnsureXr() {
    if (xr::Ready()) return true;
    if (g_xrFailed) return false;
    uint8_t* drv = *(uint8_t**)A(eng::GraphicsDriver);
    if (!drv || *(uintptr_t*)drv != A(eng::DriverVtableD3D10)) { LOG("not the D3D10 renderer; VR disabled"); g_xrFailed = true; return false; }
    if (!g_haveDevice) {
        g_swap = *(IDXGISwapChain**)(drv + 0x40);
        if (!g_swap || FAILED(g_swap->GetDevice(__uuidof(ID3D10Device), (void**)&g_dev10))) {
            LOG("could not get D3D10 device"); g_xrFailed = true; return false;
        }
        g_haveDevice = true;
        HookDeviceVtable(g_dev10);
        {
            void* engDev = *(void**)(drv + 0x38);
            LOG("engine device at drv+0x38 = %p (g_dev10 %p), vtables %p / %p", engDev, (void*)g_dev10, engDev ? *(void**)engDev : nullptr, *(void**)g_dev10);
            if (engDev && *(void**)engDev != *(void**)g_dev10) HookDrawTable(*(void***)engDev, "engine");
            InstallInlineDrawHooks();
        }
        if (g_cfg.profile) { perf::GpuInit(g_dev10); perf::StartSampler(GetCurrentThreadId()); }
    }
    static DWORD nextTry = 0;
    if (GetTickCount() < nextTry) return false;
    D3D10_TEXTURE2D_DESC bbd;
    ID3D10Texture2D* bb = AcquireBackbuffer(&bbd);
    if (!bb) return false;
    bb->Release();
    LogVA("before XR init");
    uint32_t rw = 0, rh = 0;
    if (!xr::InitInstance(g_dev10, rw, rh)) {
        if (xr::Fatal()) { LOG("OpenXR init failed: %s (VR disabled for this run)", xr::LastError()); g_xrFailed = true; return false; }
        LOG("OpenXR init failed: %s (retrying in 5 s)", xr::LastError());
        nextTry = GetTickCount() + 5000;
        return false;
    }
    if (!g_eyeTex) {
        UINT w = (UINT)(rw * g_cfg.renderScale * (1.0f + g_cfg.overscanH) + 0.5f) & ~1u, h = (UINT)(rh * g_cfg.renderScale * (1.0f + g_cfg.overscanV) + 0.5f) & ~1u;
        const float rr = dlssfc2::RenderRatio();
        UINT tw = w, th = h;
        if (rr < 0.999f) { tw = (UINT)(w * rr + 0.5f) & ~1u; th = (UINT)(h * rr + 0.5f) & ~1u; }
        if (!CreateEyeTargets(tw, th)) LOG("falling back to rendering eyes in the backbuffer (%ux%u)", bbd.Width, bbd.Height);
        else if (tw != w) {
            g_upscale = true; g_outSize[0] = (int)w; g_outSize[1] = (int)h;
            xr::SetEyeOutputSize(w, h); dlssfc2::SetOutputSize(w, h);
            LOG("DLSS upscaling: eyes render %ux%u (%.0f%%), shown %ux%u", tw, th, rr * 100.0f, w, h);
            if (IniF("DlssLodBias", 1) != 0) g_lodBias = log2f((float)tw / (float)w);
            if (afw::Wanted()) { afw::Toggle(); LOG("AFW is off with DLSS upscaling (it needs DLAA)"); }
        }
    }
    D3D10_TEXTURE2D_DESC eyed = bbd;
    if (g_eyeTex) g_eyeTex->GetDesc(&eyed);
    if (!xr::InitSwapchains(eyed, bbd)) { LOG("OpenXR swapchain init failed: %s (VR disabled for this run)", xr::LastError()); g_xrFailed = true; return false; }
    xr::SetEyeFilter(&dlssfc2::Evaluate11);
    dlssfc2::SetNotify(&ShowToast);
    g_xrW = eyed.Width; g_xrH = eyed.Height; g_scrW = bbd.Width; g_scrH = bbd.Height;
    if (g_cfg.hudLayer && xr::HudTexture() && g_eyeTex) {
        D3D10_TEXTURE2D_DESC ed; g_eyeTex->GetDesc(&ed); ed.MiscFlags = 0; ed.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(g_dev10->CreateTexture2D(&ed, nullptr, &g_preUi)) && SUCCEEDED(g_dev10->CreateRenderTargetView(g_eyeTex, nullptr, &g_eyeRTV)))
            g_hudRTV = g_eyeRTV;   // marks the HUD path as available
    }
    if (g_cfg.uiMode == 2 && xr::HudTexture() && !g_uiLayerRTV) {
        if (FAILED(g_dev10->CreateRenderTargetView(xr::HudTexture(), nullptr, &g_uiLayerRTV))) { g_uiLayerRTV = nullptr; LOG("UI layer RTV failed; falling back to UiMode 1"); g_cfg.uiMode = 1; }
        if (xr::ReticleTexture() && FAILED(g_dev10->CreateRenderTargetView(xr::ReticleTexture(), nullptr, &g_uiRetRTV))) g_uiRetRTV = nullptr;
    }
    if (g_cfg.uiMode == 2 && !g_uiLayerRTV) g_cfg.uiMode = 1;
    if (g_cfg.uiMode && !g_hudRTV) { InstallUiPatches(); LOG("UI mode %d: depth %.1f m, scale %.2f, scope %d (RSSetViewports hook %s)", g_cfg.uiMode, g_cfg.uiDepth, g_cfg.uiScale, g_cfg.uiScope, t_RSSetVP ? "on" : "MISSING"); }
    if (g_hudRTV) {
        InstallUiPatches();
        LOG("HUD layer on (distance %.2f m, %.0f deg)", g_cfg.hudDistance, g_cfg.hudFov);
    }
    LogVA("after XR init");
    return true;
}

static void Hotkeys(const XrPosef* head) {
    static bool prevR = false, prevT = false;
    bool r = (GetAsyncKeyState(g_cfg.recenterKey) & 0x8000) != 0;
    bool t = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (r && !prevR && head) { DoRecenter(*head); g_refYTime = NowMs(); }
    if (t && !prevT) { g_stereoOn = !g_stereoOn; LOG("stereo %s", g_stereoOn ? "on" : "off"); }
    { static bool prevF4 = false; bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0 && !(GetAsyncKeyState(VK_MENU) & 0x8000);   // v231: 3 s motion trace
      bool sh = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      if (f4 && !prevF4 && sh && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { g_framePacing = !g_framePacing; g_paceDebt = 0; LOG("frame pacing %d (Shift+F4)", g_framePacing); ShowToast(g_framePacing ? "Frame pacing: ON" : "Frame pacing: OFF"); }
      if (f4 && !prevF4 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) cbcap::Arm(sh); else
      if (f4 && !prevF4 && !sh && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { bool on = arms::ClavFix(true); LOG("shoulder fix %d (F4)", on); ShowToast(on ? "Shoulder fix: ON" : "Shoulder fix: OFF (old arms)"); }

      prevF4 = f4; }
    { static bool p5 = false; bool k5 = (GetAsyncKeyState(VK_F5) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000); if (k5 && !p5) { bool on = arms::LocalFrameToggle(); LOG("arm local frame %d (Ctrl+F5)", on); ShowToast(on ? "Arm precision fix: ON" : "Arm precision fix: OFF"); } p5 = k5; }
    {   // v232 test keys (jitter A/B): Insert = DLSS on/off, Page Down = DLSS debug mode (1 = no sub-pixel jitter)
        static bool pI = false, pN = false; bool i = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0, nx = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
        if (i && !pI) dlssfc2::Toggle(); if (nx && !pN) dlssfc2::CycleDebug(); pI = i; pN = nx; }
    { static bool prevF10 = false; bool f10 = (DevKey(VK_F10) & 0x8000) != 0; if (f10 && !prevF10) g_traceLeft = 4; prevF10 = f10; }
    { static bool prevF3 = false; bool f3 = (DevKey(VK_F3) & 0x8000) != 0; if (f3 && !prevF3) { g_fovMode = (g_fovMode + 1) % 3; static const char* nm[3] = { "headset frustums", "symmetric", "symmetric + overscan" }; LOG("eye frustum mode %d: %s", g_fovMode, nm[g_fovMode]); } prevF3 = f3; }
    { static bool prevF5 = false; bool f5 = (DevKey(VK_F5) & 0x8000) != 0; if (f5 && !prevF5) { g_cfg.uiScope = (g_cfg.uiScope + 1) % 4; LOG("UI scope %d", g_cfg.uiScope); } prevF5 = f5; }
    { static bool prevF6 = false; bool f6 = (DevKey(VK_F6) & 0x8000) != 0; if (f6 && !prevF6 && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { g_cullMode = (g_cullMode + 1) % 4; static const char* cn[4] = { "wide VR camera", "wide planes + flat FOV/projection, wide water reflection (default)", "game's own camera", "wide planes/projection + flat FOV (water-safe)" }; LOG("cull mode %d: %s", g_cullMode, cn[g_cullMode]); } prevF6 = f6; }
    { static bool prevF7 = false; bool f7 = (DevKey(VK_F7) & 0x8000) != 0; if (f7 && !prevF7) dlssfc2::ArmCapture(); if (f7 && !prevF7 && g_capState == 0) { g_capState = 1; LOG("capture armed"); } prevF7 = f7; }
    // test cheats (temporary): the game's own GodMode / UnlimitedAmmo switches in its game config
    // ([0x10FDECC4] = CGameConfig, DefaultGameConfig.xml: +0x94 GodMode, +0x98 UnlimitedAmmo; read by the game as dwords)
    {
        static bool cheats = g_cfg.testCheats != 0, prevF11 = false, logged = false;
        bool k = (DevKey(VK_F11) & 0x8000) != 0;
        if (k && !prevF11 && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) { arms::RequestGiveThrowables(); ShowToast("Grenades + molotovs: filling up"); prevF11 = k; } else
        if (k && !prevF11) { cheats = !cheats; ShowToast(cheats ? "God mode + unlimited ammo ON" : "God mode + unlimited ammo OFF"); LOG("test cheats %s", cheats ? "on" : "off"); }
        prevF11 = k;
        uint8_t** pc = (uint8_t**)A(0x10FDECC4);
        uint8_t* gc = pc && !IsBadReadPtr(pc, 4) ? *pc : nullptr;
        if (gc && !IsBadWritePtr(gc + 0x94, 8)) {
            static bool was = false;
            if (cheats) { *(int*)(gc + 0x94) = 1; *(int*)(gc + 0x98) = 1; }
            else if (was) { *(int*)(gc + 0x94) = 0; *(int*)(gc + 0x98) = 0; }
            was = cheats;
            if (!logged && cheats) { logged = true; LOG("test cheats: game config %p, GodMode and UnlimitedAmmo set", gc); }
        }
    }
    // v202: Insert (DLSS), Delete (AFW), Page Up (DLSS preset) and Page Down (DLSS test modes) removed - set in the launcher
    { static bool prevF11 = false; bool f11 = (DevKey(VK_F11) & 0x8000) != 0; if (f11 && !prevF11) { g_treeCache = !g_treeCache; LOG("tree render-data cache %s", g_treeCache ? "ON" : "off"); } prevF11 = f11; }
    prevR = r; prevT = t;
}


// ---- F7 capture: save the left-eye image and the backbuffer as BMPs and log every viewport call in that frame.
static void SaveTexBmp(ID3D10Texture2D* src, const char* name, int step) {
    if (!src || !g_dev10) return;
    D3D10_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) { LOG("capture %s: MSAA texture skipped", name); return; }
    D3D10_TEXTURE2D_DESC sd = d; sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1;
    ID3D10Texture2D* st = nullptr;
    if (FAILED(g_dev10->CreateTexture2D(&sd, nullptr, &st))) { LOG("capture %s: staging failed", name); return; }
    g_dev10->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
    D3D10_MAPPED_TEXTURE2D m;
    if (SUCCEEDED(st->Map(0, D3D10_MAP_READ, 0, &m))) {
        int w = (int)d.Width / step, h = (int)d.Height / step, row = (w * 3 + 3) & ~3;
        static int capN = 0; if (strstr(name, "eye0")) ++capN;
        char path[MAX_PATH]; snprintf(path, MAX_PATH, "%s%d_%s", g_dir, capN, name);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + row * h; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = w; *(int32_t*)(hdr + 22) = h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24;
            fwrite(hdr, 1, 54, f);
            uint8_t* line = (uint8_t*)calloc(row, 1);
            bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (int y = h - 1; y >= 0; --y) {
                const uint8_t* sr = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch;
                bool half4 = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, f4 = d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
                int bpp = half4 ? 8 : f4 ? 16 : 4;
                auto h2f = [](uint16_t h) -> float { uint32_t e = (h >> 10) & 31, m = h & 1023; float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : ldexpf(1.0f + m / 1024.0f, (int)e - 15); return (h & 0x8000) ? -v : v; };
                auto to8 = [](float v) -> uint8_t { v = v / (1.0f + v); if (!(v > 0)) v = 0; return (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f); };   // tone-mapped
                for (int x = 0; x < w; ++x) { const uint8_t* px = sr + (size_t)x * step * bpp;
                    if (half4 || f4) {
                        float c[4]; for (int k = 0; k < 4; ++k) c[k] = half4 ? h2f(((const uint16_t*)px)[k]) : ((const float*)px)[k];
                        if (strstr(name, "alpha")) { line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = to8(c[3]); continue; }
                        line[x * 3 + 0] = to8(c[2]); line[x * 3 + 1] = to8(c[1]); line[x * 3 + 2] = to8(c[0]); continue;
                    }
                    if (strstr(name, "alpha")) { line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = px[3]; continue; }
                    line[x * 3 + 0] = bgra ? px[0] : px[2]; line[x * 3 + 1] = px[1]; line[x * 3 + 2] = bgra ? px[2] : px[0]; }
                fwrite(line, 1, row, f);
            }
            free(line); fclose(f);
            LOG("capture: wrote %s (%dx%d from %ux%u fmt=%d)", name, w, h, d.Width, d.Height, (int)d.Format);
        }
        st->Unmap(0);
    }
    st->Release();
}
typedef void (__fastcall *Render2DView_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Render2DView_t o_Render2DView = nullptr;
static void __fastcall Hook_Render2DView(void* self, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    if (g_capState == 2) LogWrite("capture: Render2DView self=%p a=%08x %08x %08x %08x %08x", self, a1, a2, a3, a4, a5);
    o_Render2DView(self, edx, a1, a2, a3, a4, a5);
}

// ---- HUD / menus as a separate layer. The game draws its UI at the end of every 3D dispatch, inside the frame
// execute function 0x10355F60: 2DPostFx pass, two UI renderers, Text pass (0x10356B44 .. 0x10356BAE). Mid-function
// patches bracket that tail: in eye 0 the UI is redirected into an eye-sized transparent HUD texture, in eye 1 it
// is discarded (no render target bound). The HUD is then shown as a quad along the gun's aim direction.
static int g_uiEye = -1;                          // eye being dispatched (-1 = not an eye pass)
static ID3D10RenderTargetView* g_uiSavedRTV = nullptr; static ID3D10DepthStencilView* g_uiSavedDSV = nullptr;
static bool g_uiRedirected = false;
static bool g_uiActive = false;
static bool g_hudFresh = false;
static DXGI_FORMAT TypelessFamily10(DXGI_FORMAT f) {
    switch (f) { case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS; default: return f; }
}
static ID3D10Texture2D* g_uiScene = nullptr;      // the scene texture the UI composite draws as its background
static ID3D10RenderTargetView* g_uiSceneRTV = nullptr;
// Find the finished-scene texture: the eye-sized shader resource bound to the pixel shader stage when the UI
// starts, or failing that the currently bound eye-sized render target that is not the eye target.
// The game renders with MSAA: when the UI starts, the bound target is the multisampled scene buffer. The UI is
// drawn into it, and the engine resolves it into the eye texture afterwards (0x1037F860 / end of the dispatch).
static ID3D10Texture2D* g_uiTarget = nullptr; static UINT g_uiSamples = 1; static DXGI_FORMAT g_uiFmt = DXGI_FORMAT_UNKNOWN;
static bool g_uiIsolated[2] = { false, false };
// Scope view: the scene is kept aside when the UI starts and the UI is drawn onto black, so the ammo counter can be shown
// on its own (no world behind it); the scope window uses the kept scene.

// v231 diagnostic: mean brightness of the centre 16x16 of a texture (R11G11B10_FLOAT or 8-bit RGBA), -1 if unreadable
static float DecodeF11(uint32_t v, int mbits) { uint32_t e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1); if (!e) return (float)m / (1 << mbits) * 6.1035e-5f; if (e == 31) return 65504.0f; return ldexpf(1.0f + (float)m / (1 << mbits), (int)e - 15); }
static float CentreLuma(ID3D10Texture2D* t) {
    if (!t || !g_dev10) return -1;
    D3D10_TEXTURE2D_DESC d; t->GetDesc(&d);
    if (d.SampleDesc.Count > 1 || d.Width < 16 || d.Height < 16) return -1;
    static ID3D10Texture2D* st = nullptr; static DXGI_FORMAT stf = DXGI_FORMAT_UNKNOWN;
    if (!st || stf != d.Format) { if (st) st->Release(); st = nullptr;
        D3D10_TEXTURE2D_DESC sd = d; sd.Width = sd.Height = 16; sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(g_dev10->CreateTexture2D(&sd, nullptr, &st))) { st = nullptr; return -1; } stf = d.Format; }
    D3D10_BOX b{ d.Width / 2 - 8, d.Height / 2 - 8, 0, d.Width / 2 + 8, d.Height / 2 + 8, 1 };
    g_dev10->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, &b);
    D3D10_MAPPED_TEXTURE2D m; if (FAILED(st->Map(0, D3D10_MAP_READ, 0, &m))) return -1;
    double sum = 0;
    for (int y = 0; y < 16; ++y) { const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + y * m.RowPitch);
        for (int x = 0; x < 16; ++x) { uint32_t v = row[x]; float r, g, bl;
            if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT) { r = DecodeF11(v & 0x7FF, 6); g = DecodeF11((v >> 11) & 0x7FF, 6); bl = DecodeF11(v >> 22, 5); }
            else { r = (v & 255) / 255.0f; g = ((v >> 8) & 255) / 255.0f; bl = ((v >> 16) & 255) / 255.0f; }
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * bl; } }
    st->Unmap(0);
    return (float)(sum / 256.0);
}
static ID3D10Texture2D* g_scopePre = nullptr; static bool g_scopePreValid = false; static ID3D10Texture2D* g_scopeComp = nullptr; static ID3D10Texture2D* g_scopeBbTmp = nullptr;
static bool ScopeUiBegin() {
    if (!g_scopeBBres || !g_cfg.scopeAmmoMask || !g_dev10) return false;
    ID3D10RenderTargetView* rtv = nullptr; ID3D10DepthStencilView* dsv = nullptr;
    g_dev10->OMGetRenderTargets(1, &rtv, &dsv); if (dsv) dsv->Release();
    if (!rtv) return false;
    ID3D10Resource* r = nullptr; rtv->GetResource(&r); ID3D10Texture2D* t = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t); r->Release(); }
    D3D10_TEXTURE2D_DESC d{}; if (t) t->GetDesc(&d);
    static int logged = 0;
    bool ok = t && d.Width == g_scrW && d.Height == g_scrH;
    if (logged < 6) { ++logged; LOG("scope UI begin: target %ux%u fmt=%d samples=%u %s", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, ok ? "-> scene kept" : "-> left alone"); }
    if (ok) {
        if (g_scopePre) { D3D10_TEXTURE2D_DESC pd; g_scopePre->GetDesc(&pd); if (pd.Width != d.Width || pd.Height != d.Height || pd.Format != d.Format) { g_scopePre->Release(); g_scopePre = nullptr; if (g_scopeComp) { g_scopeComp->Release(); g_scopeComp = nullptr; } } }
        if (!g_scopePre) { D3D10_TEXTURE2D_DESC pd = d; pd.SampleDesc.Count = 1; pd.SampleDesc.Quality = 0; pd.MipLevels = 1; pd.ArraySize = 1; pd.Usage = D3D10_USAGE_DEFAULT; pd.BindFlags = D3D10_BIND_SHADER_RESOURCE; pd.CPUAccessFlags = 0; pd.MiscFlags = 0;
            if (FAILED(g_dev10->CreateTexture2D(&pd, nullptr, &g_scopePre))) g_scopePre = nullptr;
            else if (FAILED(g_dev10->CreateTexture2D(&pd, nullptr, &g_scopeComp))) g_scopeComp = nullptr; }
        if (g_scopePre && g_scopeComp) {
            if (d.SampleDesc.Count > 1) g_dev10->ResolveSubresource(g_scopePre, 0, t, 0, d.Format); else g_dev10->CopyResource(g_scopePre, t);
            g_scopePreValid = true;
        } else ok = false;
    }
    if (t) t->Release(); rtv->Release();
    return ok;
}
// The ammo counter alone: pixels of the finished frame that differ from the kept scene are the HUD, the rest becomes
// black. Done on the CPU over the small counter area, one frame late (two staging copies in turn, so nothing waits).
static ID3D10Texture2D* g_akSt[2][2] = {}; static ID3D10Texture2D* g_akOut = nullptr; static UINT g_akW = 0, g_akH = 0; static int g_akIdx = 0; static bool g_akHave[2] = {};
static std::vector<uint32_t> g_akBuf;
static void ScopeAmmoKey(ID3D10Texture2D* frame, const D3D10_BOX& box) {
    UINT w = box.right - box.left, h = box.bottom - box.top;
    D3D10_TEXTURE2D_DESC pd; g_scopePre->GetDesc(&pd);
    if (w != g_akW || h != g_akH) {
        for (auto& a : g_akSt) for (auto& t : a) if (t) { t->Release(); t = nullptr; }
        if (g_akOut) { g_akOut->Release(); g_akOut = nullptr; }
        g_akHave[0] = g_akHave[1] = false; g_akW = w; g_akH = h;
        D3D10_TEXTURE2D_DESC sd = pd; sd.Width = w; sd.Height = h; sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        for (auto& a : g_akSt) for (auto& t : a) if (FAILED(g_dev10->CreateTexture2D(&sd, nullptr, &t))) t = nullptr;
        D3D10_TEXTURE2D_DESC od = pd; od.Width = w; od.Height = h; od.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev10->CreateTexture2D(&od, nullptr, &g_akOut))) g_akOut = nullptr;
        g_akBuf.assign((size_t)w * h, 0xFF000000u);
        if (g_akOut) g_dev10->UpdateSubresource(g_akOut, 0, nullptr, g_akBuf.data(), w * 4, 0);
    }
    if (!g_akOut || !g_akSt[0][0] || !g_akSt[0][1] || !g_akSt[1][0] || !g_akSt[1][1]) return;
    int cur = g_akIdx, prev = g_akIdx ^ 1; g_akIdx ^= 1;
    g_dev10->CopySubresourceRegion(g_akSt[cur][0], 0, 0, 0, 0, frame, 0, &box);
    g_dev10->CopySubresourceRegion(g_akSt[cur][1], 0, 0, 0, 0, g_scopePre, 0, &box);
    if (g_akHave[prev]) {
        D3D10_MAPPED_TEXTURE2D mf, mp;
        if (SUCCEEDED(g_akSt[prev][0]->Map(0, D3D10_MAP_READ, 0, &mf))) {
            if (SUCCEEDED(g_akSt[prev][1]->Map(0, D3D10_MAP_READ, 0, &mp))) {
                const int th = g_cfg.scopeAmmoKey;
                for (UINT y = 0; y < h; ++y) {
                    const uint32_t* f = (const uint32_t*)((const uint8_t*)mf.pData + (size_t)y * mf.RowPitch);
                    const uint32_t* q = (const uint32_t*)((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch);
                    uint32_t* o = &g_akBuf[(size_t)y * w];
                    for (UINT x = 0; x < w; ++x) {
                        uint32_t a = f[x], b = q[x]; int d = 0;
                        for (int c = 0; c < 24; c += 8) { int e = abs((int)((a >> c) & 255) - (int)((b >> c) & 255)); if (e > d) d = e; }
                        // HUD glyphs are light and nearly grey/cream; the world behind them (and the counter's
                        // see-through brown bar) is darker or more coloured. Keep only light, low-colour pixels that
                        // also differ from the world picture.
                        int r0 = a & 255, g0 = (a >> 8) & 255, b0 = (a >> 16) & 255;
                        int mn = r0 < g0 ? (r0 < b0 ? r0 : b0) : (g0 < b0 ? g0 : b0), mx = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
                        int kb = (mn - g_cfg.scopeAmmoLight) * 256 / 40; kb = kb < 0 ? 0 : kb > 256 ? 256 : kb;
                        int ks = (110 - (mx - mn)) * 256 / 40; ks = ks < 0 ? 0 : ks > 256 ? 256 : ks;
                        int kd = d >= th ? 256 : d * 256 / th;
                        int k = (kb * ks >> 8) * kd >> 8;
                        uint32_t rr = 0xFF000000u;
                        for (int c = 0; c < 24; c += 8) rr |= (uint32_t)((((a >> c) & 255) * k) >> 8) << c;
                        o[x] = rr;
                    }
                }
                g_akSt[prev][1]->Unmap(0);
                g_dev10->UpdateSubresource(g_akOut, 0, nullptr, g_akBuf.data(), w * 4, 0);
            }
            g_akSt[prev][0]->Unmap(0);
        }
    }
    g_akHave[cur] = true;
    g_dev10->CopySubresourceRegion(g_scopeComp, 0, box.left, box.top, 0, g_akOut, 0, nullptr);
}
extern "C" void __cdecl OnUiBegin() {
    if (g_capState == 2) TraceMark("UI tail begin (after 2DPostFx)");
    if (g_scopeBBres && ScopeUiBegin()) return;   // scope view: the UI stays in the flat frame; the scene without it is kept
    if (g_cfg.uiMode == 2 && g_uiEye >= 0 && g_dev10 && g_uiLayerRTV && t_OMSetRT) {
        ID3D10RenderTargetView* rtv = nullptr; ID3D10DepthStencilView* dsv = nullptr;
        g_dev10->OMGetRenderTargets(1, &rtv, &dsv);
        bool isEye = false;
        if (rtv) { ID3D10Resource* r = nullptr; rtv->GetResource(&r); isEye = r && ((void*)r == (void*)g_eyeTex || ScopeUiTarget(r)); if (r) r->Release(); rtv->Release(); }
        if (dsv) dsv->Release();
        g_uiShift = true; g_uiOnEye = isEye; g_uiRedirect = false; g_uiCurEye = g_uiEye; g_uiDrawOrd = 0;
        if (g_uiEye == g_uiPrimary) { const float z[4] = { 0, 0, 0, 0 }; g_dev10->ClearRenderTargetView(g_uiLayerRTV, z); if (g_uiRetRTV) g_dev10->ClearRenderTargetView(g_uiRetRTV, z); g_uiRetFresh = false; }
        g_uiBoundLayer = nullptr; g_blendSetFixed = nullptr;
        InterlockedIncrement(&g_uiBegins);
        return;
    }
    if (g_cfg.uiMode && g_uiEye >= 0 && g_dev10 && t_RSSetVP && t_OMSetRT) {
        // Where the aim point lands in this eye's image.
        g_uiCx = 0.5f; g_uiCy = 0.5f;
        if (g_eyeCam && g_uiAimValid) {
            const float* M = &F<float>(g_eyeCam, cam::ViewProj);
            V3 q = g_uiAim; float c[4];
            for (int j = 0; j < 4; ++j) c[j] = q.x * M[j] + q.y * M[4 + j] + q.z * M[8 + j] + M[12 + j];
            if (c[3] > 1e-4f) {
                float nx = c[0] / c[3], ny = c[1] / c[3];
                if (nx > -1.5f && nx < 1.5f && ny > -1.5f && ny < 1.5f) { g_uiCx = 0.5f + 0.5f * nx; g_uiCy = 0.5f - 0.5f * ny; }
            }
        }
        ID3D10RenderTargetView* rtv = nullptr; ID3D10DepthStencilView* dsv = nullptr;
        g_dev10->OMGetRenderTargets(1, &rtv, &dsv);
        bool rtvIsEye = false;
        if (rtv) { ID3D10Resource* r = nullptr; rtv->GetResource(&r); rtvIsEye = r && (void*)r == (void*)g_eyeTex; if (r) r->Release(); }
        g_uiShiftRTV = (uintptr_t)rtv; if (rtv) rtv->Release(); if (dsv) dsv->Release();
        UINT nv = 1; D3D10_VIEWPORT v{}; g_dev10->RSGetViewports(&nv, &v);
        g_uiVpOrig = v; g_uiVpValid = nv >= 1;
        bool onNow = g_cfg.uiScope != 2 && g_cfg.uiScope != 3;
        if (g_cfg.uiScope == 2 && rtvIsEye) onNow = true;
        g_uiOnEye = rtvIsEye; g_uiVpShifted = false;
        g_uiShift = true; g_uiShiftRT = onNow; InterlockedIncrement(&g_uiBegins);
        if (g_uiVpValid && onNow) { D3D10_VIEWPORT t = UiXform(v); t_RSSetVP(g_dev10, 1, &t); }
        static int logged = 0;
        if (logged < 4 || g_capState == 2) { ++logged; LOG("UI shift begin (eye %d): centre %.3f,%.3f vp %d,%d %ux%u rtv %08x", g_uiEye, g_uiCx, g_uiCy, v.TopLeftX, v.TopLeftY, v.Width, v.Height, (unsigned)g_uiShiftRTV); }
        return;
    }
    if (g_uiEye < 0 || !g_cfg.hudLayer || !g_dev10 || !g_eyeTex || !g_preUi) return;
    InterlockedIncrement(&g_uiBegins);
    ID3D10RenderTargetView* rtv = nullptr; ID3D10DepthStencilView* dsv = nullptr;
    g_dev10->OMGetRenderTargets(1, &rtv, &dsv);
    if (dsv) dsv->Release();
    if (!rtv) return;
    ID3D10Resource* r = nullptr; rtv->GetResource(&r);
    ID3D10Texture2D* t = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t); r->Release(); }
    D3D10_TEXTURE2D_DESC ed, d{}; g_eyeTex->GetDesc(&ed);
    if (t) t->GetDesc(&d);
    static int logged = 0;
    bool ok = t && d.Width == ed.Width && d.Height == ed.Height && TypelessFamily10(d.Format) == TypelessFamily10(ed.Format);
    if (logged < 4 || g_capState == 2) { ++logged; LOG("UI begin (eye %d): target %ux%u fmt=%d samples=%u %s", g_uiEye, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, ok ? "-> isolating" : "-> left alone"); }
    if (!ok) { if (t) t->Release(); rtv->Release(); return; }
    // Clean 3D image aside, then a transparent background for the UI.
    if (d.SampleDesc.Count > 1) g_dev10->ResolveSubresource(g_preUi, 0, t, 0, ed.Format);
    else if (t != g_eyeTex) g_dev10->CopyResource(g_preUi, t);
    else g_dev10->CopyResource(g_preUi, g_eyeTex);
    const float clear[4] = { 0, 0, 0, 0 };
    g_dev10->ClearRenderTargetView(rtv, clear);
    rtv->Release();
    if (g_uiTarget) g_uiTarget->Release();
    g_uiTarget = t; g_uiSamples = d.SampleDesc.Count; g_uiFmt = ed.Format;
    g_uiActive = true;
}
extern "C" void __cdecl OnUiEnd() {
    if (g_capState == 2) TraceMark("UI tail end (before 0x1037F860)");
    if (g_uiShift && g_cfg.uiMode == 2) {
        if (g_uiRedirect && g_uiSavedEyeRTV) t_OMSetRT(g_dev10, 1, &g_uiSavedEyeRTV, g_uiSavedEyeDSV);
        if (g_blendSetFixed) {
            ID3D10BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; g_dev10->OMGetBlendState(&bs, bf, &sm);
            if (bs == g_blendSetFixed && g_blendSetOrig) g_dev10->OMSetBlendState(g_blendSetOrig, bf, sm);
            if (bs) bs->Release();
            g_blendSetFixed = nullptr;
        }
        if (g_uiSavedEyeRTV) { g_uiSavedEyeRTV->Release(); g_uiSavedEyeRTV = nullptr; }
        if (g_uiSavedEyeDSV) { g_uiSavedEyeDSV->Release(); g_uiSavedEyeDSV = nullptr; }
        if (g_uiCurEye == g_uiPrimary && g_uiDrawOrd > 0) g_uiLayerFresh = true;
        if (g_capState == 2 && g_uiCurEye == g_uiPrimary && g_uiDrawOrd > 0) UiProbe(g_uiDrawOrd - 1);
        if (g_capState == 2) LOG("UI layer: eye %d, %d UI draws", g_uiCurEye, g_uiDrawOrd);
        g_uiShift = false; g_uiOnEye = false; g_uiRedirect = false; g_uiCurEye = -1;
        return;
    }
    if (g_uiShift) {
        g_uiShift = false; g_uiShiftRT = false; g_uiOnEye = false; g_uiVpShifted = false;
        if (g_uiVpValid && t_RSSetVP) t_RSSetVP(g_dev10, 1, &g_uiVpOrig);
        g_uiVpValid = false;
        return;
    }
    if (!g_uiActive) return;
    if (g_capState == 2) {
        ID3D10RenderTargetView* rtv = nullptr; ID3D10DepthStencilView* dsv = nullptr; g_dev10->OMGetRenderTargets(1, &rtv, &dsv);
        D3D10_TEXTURE2D_DESC d{}; ID3D10Texture2D* t = nullptr;
        if (rtv) { ID3D10Resource* r = nullptr; rtv->GetResource(&r); if (r) { r->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&t); r->Release(); } rtv->Release(); }
        if (t) { t->GetDesc(&d); }
        LOG("UI end (eye %d): bound target %p %ux%u samples=%u (begin target %p, eye %p)", g_uiEye, (void*)t, d.Width, d.Height, d.SampleDesc.Count, (void*)g_uiTarget, (void*)g_eyeTex);
        if (t) t->Release(); if (dsv) dsv->Release();
    }
    g_uiActive = false;
    if (g_uiEye == g_uiPrimary && xr::HudTexture() && g_uiTarget) {
        if (g_uiSamples > 1) g_dev10->ResolveSubresource(xr::HudTexture(), 0, g_uiTarget, 0, g_uiFmt);
        else g_dev10->CopyResource(xr::HudTexture(), g_uiTarget);
        g_hudFresh = true;
    }
    if (g_uiEye >= 0 && g_uiEye < 2) g_uiIsolated[g_uiEye] = true;   // eye image is restored after the dispatch
}
// stub: pushad; pushfd; call fn; popfd; popad; <relocated bytes>; jmp back
static bool PatchMidCall(uintptr_t site, const uint8_t* expect, size_t n, void (*fn)(), const uint8_t* reloc, size_t rn, uintptr_t relCallTarget, size_t relCallAt) {
    // bytes must match; a relocated call's displacement is checked by its target instead (it differs per build)
    bool same = site && (relCallTarget ? memcmp((void*)site, expect, relCallAt + 1) == 0 && memcmp((uint8_t*)site + relCallAt + 5, expect + relCallAt + 5, n - relCallAt - 5) == 0
                                         && site + relCallAt + 5 + *(int32_t*)(site + relCallAt + 1) == relCallTarget
                                       : memcmp((void*)site, expect, n) == 0);
    if (!same) { LOG("mid patch %08x: unexpected bytes", (unsigned)site); return false; }
    uint8_t* st = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    size_t k = 0;
    st[k++] = 0x60; st[k++] = 0x9C;
    st[k++] = 0xE8; *(int32_t*)(st + k) = (int32_t)((uintptr_t)fn - ((uintptr_t)st + k + 4)); k += 4;
    st[k++] = 0x9D; st[k++] = 0x61;
    size_t relocAt = k; memcpy(st + k, reloc, rn); k += rn;
    if (relCallTarget) *(int32_t*)(st + relocAt + relCallAt + 1) = (int32_t)(relCallTarget - ((uintptr_t)st + relocAt + relCallAt + 5));
    st[k++] = 0xE9; *(int32_t*)(st + k) = (int32_t)((site + n) - ((uintptr_t)st + k + 4)); k += 4;
    FlushInstructionCache(GetCurrentProcess(), st, 64);
    DWORD old; VirtualProtect((void*)site, n, PAGE_EXECUTE_READWRITE, &old);
    uint8_t j[16]; memset(j, 0x90, sizeof j); j[0] = 0xE9; *(int32_t*)(j + 1) = (int32_t)((uintptr_t)st - (site + 5));
    memcpy((void*)site, j, n);
    VirtualProtect((void*)site, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, n);
    return true;
}
static void InstallUiPatches() {
    // Begin after the 2DPostFx pass (it redraws the whole 3D image), i.e. before the two UI renderers + Text.
    static const uint8_t a[8] = { 0x8B, 0x7C, 0x24, 0x30, 0x8B, 0x5C, 0x24, 0x34 };     // mov edi,[esp+30h]; mov ebx,[esp+34h]
    static const uint8_t b[8] = { 0x8B, 0x4E, 0x38, 0xE8, 0x00, 0x00, 0x00, 0x00 };     // mov ecx,[esi+38h]; call 1037F860 (displacement set in the stub)
    bool ok1 = PatchMidCall(A(0x10356B5B), a, 8, (void(*)())&OnUiBegin, a, 8, 0, 0);
    bool ok2 = PatchMidCall(A(0x10356BAE), b, 8, (void(*)())&OnUiEnd, b, 8, A(0x1037F860), 3);
    LOG("UI layer patches: begin=%d end=%d", ok1, ok2);
}
// Quaternion from an orthonormal basis (columns x, y, z).
static XrQuaternionf QuatFromBasis(V3 x, V3 y, V3 z) {
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22; XrQuaternionf q;
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s; }
    else if (m00 > m11 && m00 > m22) { float s = sqrtf(1.0f + m00 - m11 - m22) * 2; q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s; }
    else if (m11 > m22) { float s = sqrtf(1.0f + m11 - m00 - m22) * 2; q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s; }
    else { float s = sqrtf(1.0f + m22 - m00 - m11) * 2; q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s; }
    return q;
}
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
// Our own ammo counter for the scope view: "Round" with the rounds in the clip, then the spare rounds, in the
// game's HUD colours on a transparent background.
static void DrawAmmo(int clip, int pouch) {
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t* bits = nullptr; static HFONT fBig = nullptr, fSmall = nullptr;
    const int W = xr::AmmoW, H = xr::AmmoH;
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0); SelectObject(dc, bmp);
        fBig = CreateFontA(64, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        fSmall = CreateFontA(38, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        SetBkMode(dc, TRANSPARENT);
    }
    if (!bits) return;
    RECT all{ 0, 0, W, H }; FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    // three runs drawn in white (luminance = coverage), coloured below: label grey, clip cream, pouch dimmer cream
    char cl[16], po[16]; snprintf(cl, sizeof cl, "%d", clip < 0 ? 0 : clip); snprintf(po, sizeof po, "/ %d", pouch < 0 ? 0 : pouch);
    SetTextColor(dc, RGB(255, 255, 255));
    SIZE s1, s2, s3; SelectObject(dc, fSmall); GetTextExtentPoint32A(dc, "Round", 5, &s1); GetTextExtentPoint32A(dc, po, (int)strlen(po), &s3);
    SelectObject(dc, fBig); GetTextExtentPoint32A(dc, cl, (int)strlen(cl), &s2);
    int gap = 16, total = s1.cx + gap + s2.cx + gap + s3.cx, x = (W - total) / 2; if (x < 4) x = 4;
    int base = H / 2 + 22;   // common baseline
    int xLabel = x, xClip = x + s1.cx + gap, xPouch = xClip + s2.cx + gap;
    SetTextAlign(dc, TA_BASELINE | TA_LEFT);
    SelectObject(dc, fSmall); TextOutA(dc, xLabel, base, "Round", 5);
    SelectObject(dc, fBig); TextOutA(dc, xClip, base, cl, (int)strlen(cl));
    SelectObject(dc, fSmall); TextOutA(dc, xPouch, base, po, (int)strlen(po));
    GdiFlush();
    static uint32_t out[xr::AmmoW * xr::AmmoH];
    for (int y = 0; y < H; ++y) for (int xx = 0; xx < W; ++xx) {
        uint32_t p = bits[y * W + xx]; int a = (int)((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3;
        int r, g, b; if (xx < xClip - gap / 2) { r = 200; g = 200; b = 190; } else if (xx < xPouch - gap / 2) { r = 238; g = 224; b = 176; } else { r = 205; g = 192; b = 150; }
        out[y * W + xx] = ((uint32_t)a << 24) | ((uint32_t)(b * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(r * a / 255);
    }
    xr::SetAmmoImage(out);
}
// ---- End key: machete hand menu (pitch / yaw / roll sliders), shown as a panel in front of you.
// Keyboard: Up/Down select, Left/Right adjust (Shift = 1 deg steps, else 5). Controllers: left stick up/down selects,
// right stick left/right adjusts (faster the further you push). Changes show live; closing saves them to fc2vr.ini.
static const char* kMenuPages[4] = { "Machete hand", "Map & GPS hands", "Free hands", "Phone hand (calls)" };
static void DrawMenu(int page, int sel, const float v[3]) {
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t* bits = nullptr; static HFONT big = nullptr, small_ = nullptr;
    const int W = xr::MenuW, H = xr::MenuH;
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0); SelectObject(dc, bmp);
        big = CreateFontA(28, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        small_ = CreateFontA(19, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        SetBkMode(dc, TRANSPARENT);
    }
    if (!bits) return;
    RECT all{ 0, 0, W, H }; HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24)); FillRect(dc, &all, bg); DeleteObject(bg);
    // row 0: which item
    {
        int y = 14;
        if (sel == 0) { RECT hl{ 10, y - 4, W - 10, y + 38 }; HBRUSH br = CreateSolidBrush(RGB(60, 52, 36)); FillRect(dc, &hl, br); DeleteObject(br); }
        SelectObject(dc, big); SetTextColor(dc, sel == 0 ? RGB(255, 240, 200) : RGB(235, 225, 190));
        char t[64]; snprintf(t, sizeof t, "<  %s  >", kMenuPages[page]);
        RECT r{ 20, y, W - 20, y + 34 }; DrawTextA(dc, t, -1, &r, DT_CENTER | DT_SINGLELINE);
    }
    static const char* names[3] = { "Pitch", "Yaw", "Roll" };
    for (int i = 0; i < 3; ++i) {
        int row = i + 1, y = 66 + i * 58;
        if (row == sel) { RECT hl{ 10, y - 6, W - 10, y + 46 }; HBRUSH br = CreateSolidBrush(RGB(60, 52, 36)); FillRect(dc, &hl, br); DeleteObject(br); }
        SelectObject(dc, big); SetTextColor(dc, row == sel ? RGB(255, 240, 200) : RGB(190, 190, 190));
        RECT n{ 24, y, 130, y + 34 }; DrawTextA(dc, names[i], -1, &n, DT_LEFT | DT_SINGLELINE);
        char val[16]; snprintf(val, sizeof val, "%.0f\xB0", v[i]);
        RECT vr{ W - 110, y, W - 24, y + 34 }; DrawTextA(dc, val, -1, &vr, DT_RIGHT | DT_SINGLELINE);
        int x0 = 140, x1 = W - 125, cy = y + 18;   // slider: -180..180
        RECT tr{ x0, cy - 3, x1, cy + 3 }; HBRUSH tb = CreateSolidBrush(RGB(90, 90, 90)); FillRect(dc, &tr, tb); DeleteObject(tb);
        RECT zr{ (x0 + x1) / 2 - 1, cy - 9, (x0 + x1) / 2 + 1, cy + 9 }; HBRUSH zb = CreateSolidBrush(RGB(120, 120, 120)); FillRect(dc, &zr, zb); DeleteObject(zb);
        float f = (fmaxf(-180.0f, fminf(180.0f, v[i])) + 180.0f) / 360.0f; int kx = x0 + (int)(f * (x1 - x0));
        RECT kr{ kx - 6, cy - 12, kx + 6, cy + 12 }; HBRUSH kb = CreateSolidBrush(row == sel ? RGB(240, 200, 90) : RGB(200, 200, 200)); FillRect(dc, &kr, kb); DeleteObject(kb);
    }
    SelectObject(dc, small_); SetTextColor(dc, RGB(170, 170, 170));
    RECT h1{ 20, H - 60, W - 20, H - 38 }; DrawTextA(dc, "Left stick / Up-Down: choose    Right stick / Left-Right: adjust", -1, &h1, DT_LEFT | DT_SINGLELINE);
    RECT h2{ 20, H - 34, W - 20, H - 12 }; DrawTextA(dc, "Shift = 1\xB0 steps    End: close and save", -1, &h2, DT_LEFT | DT_SINGLELINE);
    GdiFlush();
    static uint32_t out[xr::MenuW * xr::MenuH];
    for (int i = 0; i < W * H; ++i) { uint32_t p = bits[i]; out[i] = 0xF0000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF); }   // BGRA -> RGBA, nearly opaque
    xr::SetMenu(out, g_menuPoseX, 0.6f);
}
// short on-screen notes (DLSS mode), shown on the menu layer for a moment
static char g_toast[128] = ""; static double g_toastUntil = 0; static bool g_toastNew = false; static XrPosef g_toastPose{};
static void ShowToast(const char* t) { snprintf(g_toast, sizeof g_toast, "%s", t); g_toastUntil = NowMs() + 2500.0; g_toastNew = true; }
static void DrawToast() {
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t* bits = nullptr; static HFONT big = nullptr;
    const int W = xr::MenuW, H = xr::MenuH;
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0); SelectObject(dc, bmp);
        big = CreateFontA(30, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        SetBkMode(dc, TRANSPARENT);
    }
    if (!bits) return;
    RECT all{ 0, 0, W, H }; HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &all, bg); DeleteObject(bg);
    RECT box{ 0, H / 2 - 40, W, H / 2 + 40 }; HBRUSH bb = CreateSolidBrush(RGB(24, 24, 24)); FillRect(dc, &box, bb); DeleteObject(bb);
    SelectObject(dc, big); SetTextColor(dc, RGB(255, 240, 200));
    RECT tr{ 10, H / 2 - 40, W - 10, H / 2 + 40 }; DrawTextA(dc, g_toast, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    GdiFlush();
    static uint32_t out[xr::MenuW * xr::MenuH];
    for (int i = 0; i < W * H; ++i) { uint32_t p = bits[i]; bool in = i / W >= H / 2 - 40 && i / W < H / 2 + 40;
        out[i] = in ? (0xF0000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF)) : 0u; }
    xr::SetMenu(out, g_toastPose, 0.6f);
}
static void ToastTick(const xr::FrameViews& fv) {
    if (!g_toast[0] || NowMs() > g_toastUntil) return;
    if (g_toastNew) {
        g_toastNew = false;
        const XrPosef& h = fv.views[0].pose;
        V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
        V3 fwd = QRot(h.orientation, { 0, 0, -1 }); fwd.y = 0; fwd = Norm(fwd);
        V3 zq = fwd * -1.0f, xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
        g_toastPose.orientation = QuatFromBasis(xq, yq, zq);
        V3 p = mid + fwd * 0.75f + V3{ 0, -0.15f, 0 }; g_toastPose.position = { p.x, p.y, p.z };
        DrawToast();
    } else xr::SetMenu(nullptr, g_toastPose, 0.6f);
}
// End key: hand angle menu. Page 1 = machete (right hand), page 2 = map & GPS (both hands, mirrored for the left).
static void MacheteMenu(const xr::FrameViews& fv) {
    static bool open = false, prevEnd = false, prevUp = false, prevDn = false, prevL = false, prevR = false; static int sel = 1, page = 0; static int stickSel = 0, stickPage = 0;
    static double tPrev = 0; static float acc = 0;
    bool endK = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    bool toggle = endK && !prevEnd; prevEnd = endK;
    auto get = [&](int pg, float* v) { if (pg == 0) arms::GetMachete(v[0], v[1], v[2]); else if (pg == 1) arms::GetMapHands(v[0], v[1], v[2]); else if (pg == 2) arms::GetFreeHands(v[0], v[1], v[2]); else arms::GetPhoneHands(v[0], v[1], v[2]); };
    auto set = [&](int pg, const float* v) { if (pg == 0) arms::SetMachete(v[0], v[1], v[2]); else if (pg == 1) arms::SetMapHands(v[0], v[1], v[2]); else if (pg == 2) arms::SetFreeHands(v[0], v[1], v[2]); else arms::SetPhoneHands(v[0], v[1], v[2]); };
    float v[3]; get(page, v);
    if (toggle) {
        open = !open; controls::SetMenuMode(open);
        if (open) {   // panel 0.75 m ahead of the head, level, facing you
            page = controls::MapOpen() ? 1 : (controls::PhoneOut() || !controls::NormalControls()) ? 3 : arms::CurrentWeapon() ? 0 : 2; get(page, v);
            const XrPosef& h = fv.views[0].pose;
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            V3 fwd = QRot(h.orientation, { 0, 0, -1 }); fwd.y = 0; fwd = Norm(fwd);
            V3 zq = fwd * -1.0f, xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
            g_menuPoseX.orientation = QuatFromBasis(xq, yq, zq);
            V3 p = mid + fwd * 0.75f + V3{ 0, -0.1f, 0 }; g_menuPoseX.position = { p.x, p.y, p.z };
            DrawMenu(page, sel, v); LOG("hand menu open (%s)", kMenuPages[page]);
        } else {
            char path[MAX_PATH], b[32]; snprintf(path, MAX_PATH, "%sfc2vr.ini", g_dir);
            float m[3], g[3], fh[3], ph[3]; get(0, m); get(1, g); get(2, fh); get(3, ph);
            static const char* keys[4][3] = { { "MacheteAngle", "MacheteYaw", "MacheteRoll" }, { "MapPitch", "MapYaw", "MapRoll" }, { "FreeHandPitch", "FreeHandYaw", "FreeHandRoll" }, { "PhonePitch", "PhoneYaw", "PhoneRoll" } };
            for (int k = 0; k < 3; ++k) { snprintf(b, sizeof b, "%.0f", m[k]); WritePrivateProfileStringA("VR", keys[0][k], b, path);
                                          snprintf(b, sizeof b, "%.0f", g[k]); WritePrivateProfileStringA("VR", keys[1][k], b, path);
                                          snprintf(b, sizeof b, "%.0f", fh[k]); WritePrivateProfileStringA("VR", keys[2][k], b, path);
                                          snprintf(b, sizeof b, "%.0f", ph[k]); WritePrivateProfileStringA("VR", keys[3][k], b, path); }
            LOG("hand menu closed: machete %.0f/%.0f/%.0f, map %.0f/%.0f/%.0f, free hands %.0f/%.0f/%.0f, phone %.0f/%.0f/%.0f saved", m[0], m[1], m[2], g[0], g[1], g[2], fh[0], fh[1], fh[2], ph[0], ph[1], ph[2]);
        }
    }
    if (!open) { tPrev = 0; ToastTick(fv); return; }
    double now = NowMs(); float dt = tPrev > 0 ? (float)((now - tPrev) * 0.001) : 0.0f; tPrev = now; if (dt > 0.1f) dt = 0.1f;
    bool changed = false;
    bool up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0, dn = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
    bool kl = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0, kr = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (up && !prevUp) { sel = (sel + 3) % 4; changed = true; }
    if (dn && !prevDn) { sel = (sel + 1) % 4; changed = true; }
    float step = shift ? 1.0f : 5.0f;
    int flip = 0;
    if (kl && !prevL) { if (sel == 0) flip = -1; else v[sel - 1] -= step; changed = true; }
    if (kr && !prevR) { if (sel == 0) flip = 1; else v[sel - 1] += step; changed = true; }
    prevUp = up; prevDn = dn; prevL = kl; prevR = kr;
    const xr::HandState& L = xr::Hand(0); const xr::HandState& R = xr::Hand(1);
    int ss = L.stick.y > 0.7f ? -1 : L.stick.y < -0.7f ? 1 : 0;
    if (ss != 0 && stickSel == 0) { sel = (sel + 4 + ss) % 4; changed = true; xr::Haptic(0, 0.2f, 0.02f); }
    stickSel = ss;
    float rx = R.stick.x; if (fabsf(rx) < 0.15f) { rx = 0; acc = 0; }
    if (sel == 0) {   // the item row: a push of the right stick flips the page
        int sp = rx > 0.6f ? 1 : rx < -0.6f ? -1 : 0;
        if (sp != 0 && stickPage == 0) { flip = sp; changed = true; xr::Haptic(1, 0.2f, 0.02f); }
        stickPage = sp;
    } else if (rx != 0) {   // up to 60 deg/s, whole degrees
        acc += rx * fabsf(rx) * 60.0f * dt;
        float whole = truncf(acc); if (whole != 0) { v[sel - 1] += whole; acc -= whole; changed = true; }
    }
    if (flip) { page = (page + 4 + flip) % 4; get(page, v); }
    for (float& x : v) { while (x > 180.0f) x -= 360.0f; while (x < -180.0f) x += 360.0f; }
    if (changed) { if (!flip) set(page, v); DrawMenu(page, sel, v); }
    else xr::SetMenu(nullptr, g_menuPoseX, 0.6f);
}
// v224 Home: smooth turning speed. Left arrow slower, right arrow faster (10% steps); Home again closes and saves.
static bool g_turnMenuOpen = false;
static void TurnMenu() {
    static bool prevH = false, prevL = false, prevR = false;
    bool h = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0, kl = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0, kr = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    auto show = [](bool saved) { char t[96]; int pct = (int)lroundf(controls::TurnSpeed() * 100.0f);
        if (saved) snprintf(t, sizeof t, "Smooth turning speed %d%% - saved", pct);
        else snprintf(t, sizeof t, "<  Smooth turning speed %d%%  >", pct);
        ShowToast(t); if (!saved) g_toastUntil = NowMs() + 600000.0; };
    if (h && !prevH) {
        g_turnMenuOpen = !g_turnMenuOpen;
        if (g_turnMenuOpen) { show(false); LOG("turn speed menu open (%.0f%%)", controls::TurnSpeed() * 100.0f); }
        else { char path[MAX_PATH], b[32]; snprintf(path, MAX_PATH, "%sfc2vr.ini", g_dir); snprintf(b, sizeof b, "%.2f", controls::TurnSpeed());
            WritePrivateProfileStringA("VR", "SmoothTurnSpeed", b, path); show(true); LOG("turn speed %.0f%% saved", controls::TurnSpeed() * 100.0f); }
    }
    if (g_turnMenuOpen) {
        float k = controls::TurnSpeed();
        if (kl && !prevL) { controls::SetTurnSpeed(lroundf(k * 10.0f - 1.0f) / 10.0f); show(false); }
        if (kr && !prevR) { controls::SetTurnSpeed(lroundf(k * 10.0f + 1.0f) / 10.0f); show(false); }
    }
    prevH = h; prevL = kl; prevR = kr;
}
static void SubmitArmsFrame(const xr::FrameViews& fv, uint8_t* rc, const PoseFrame& frame) {
    const float s = frame.worldScale;
    TurnMenu();
    MacheteMenu(fv);
    {   // controller pose for the arms/weapon and shots (game thread reads it)
        arms::Frame af; const xr::HandState& rh = xr::Hand(1);
        xr::HandState physical[2]; XrPosef inputHead; bool inputHeadValid = false;
        const bool inputCurrent = xr::InputSnapshot(physical,inputHead,inputHeadValid,&af.source);
        af.handAuthority = inputCurrent && physical[1].gripValid && physical[1].aimValid;
        af.leftAuthority = inputCurrent && physical[0].gripValid && physical[0].aimValid;
        af.headAuthority = inputCurrent && inputHeadValid;
        af.weapon = arms::CurrentWeapon();
        af.valid = true; af.handValid = rh.aimValid && rh.gripValid;
        auto put = [](float* d, V3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; };
        af.game = frame.game; af.frameSequence = frame.sequence;
        af.bonePubValid = arms::CamBonePos(af.bonePub);   // v231 body clock
        if (g_motionDiag) {   // v231 diagnostic (MotionDiag=1): does the skeleton (as the renderer will read it) still hold the hand the mod wrote?
            static int frames = 0, differ = 0; static float worst = 0; static double lastL = 0;
            float aw[3], lw[3], hd3[3], tg[3], lh[3]; long ds = 0;
            if (arms::DiagHand(aw, lw, hd3, tg, &ds) && arms::DiagLiveHand(lh)) {
                float dx = lh[0] - hd3[0], dy = lh[1] - hd3[1], dz = lh[2] - hd3[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                ++frames; if (d > 0.005f) { ++differ; if (d > worst) worst = d; }
            }
            double tn = NowMs();
            {   static int af = 0, adf = 0, hist[512] = {}; static float aw = 0; int dn = 0, wb = -1; float wm = 0;
                if (arms::DiagArmLive(&dn, &wb, &wm)) { ++af; if (dn) { ++adf; if (wb >= 0 && wb < 512) ++hist[wb]; if (wm > aw) aw = wm; } }
                if (tn - lastL > 10000.0 && af) {
                    char top[256] = ""; int o = 0;
                    for (int r = 0; r < 6; ++r) { int bi = -1, bv = 0; for (int i = 0; i < 512; ++i) if (hist[i] > bv) { bv = hist[i]; bi = i; } if (bi < 0) break; o += snprintf(top + o, sizeof top - o, " bone %d x%d", bi, bv); hist[bi] = 0; }
                    LOG("arm bone check: in %d of %d frames some arm bones differed from what the mod wrote (worst %.0f mm); most often:%s", adf, af, aw, top);
                    af = adf = 0; aw = 0; for (auto& h : hist) h = 0; } }
            if (tn - lastL > 10000.0 && frames) { LOG("skeleton check: in %d of %d frames the skeleton held a different right hand than the mod wrote (up to %.0f mm)", differ, frames, worst * 1000.0f); lastL = tn; frames = differ = 0; worst = 0; }
        }
        if (g_motionTrace > 0) {   // v231 motion trace: one line per frame
            static double t0 = 0; static long lastSeq = 0; static V3 lastRaw{}, lastBone{}, lastPl{};
            double tn = NowMs(); if (g_motionTrace == 270) { t0 = tn; lastSeq = arms::CamBoneSeq(); lastRaw = { frame.raw.P.x, frame.raw.P.y, frame.raw.P.z }; }
            float pl[3] = {}, hd = 0; bool plOk = arms::PlayerState(pl, &hd);
            long seq = arms::CamBoneSeq();
            V3 raw{ frame.raw.P.x, frame.raw.P.y, frame.raw.P.z }, bone{ af.bonePub[0], af.bonePub[1], af.bonePub[2] }, pp{ pl[0], pl[1], pl[2] };
            V3 dr = raw - lastRaw, db = bone - lastBone, dp = pp - lastPl;
            LOG("mt %6.1f ms | raw %.3f %.3f %.3f d %+6.1f mm | eff-raw %+6.1f %+6.1f %+6.1f mm | bone-raw %+6.1f %+6.1f %+6.1f mm (bone d %+6.1f mm, %ld upd) | player-raw %+6.1f %+6.1f %+6.1f mm (d %+6.1f mm)%s veh %d",
                tn - t0, raw.x, raw.y, raw.z, sqrtf(dr.x * dr.x + dr.y * dr.y + dr.z * dr.z) * 1000.0f,
                (frame.game.P.x - raw.x) * 1000.0f, (frame.game.P.y - raw.y) * 1000.0f, (frame.game.P.z - raw.z) * 1000.0f,
                (bone.x - raw.x) * 1000.0f, (bone.y - raw.y) * 1000.0f, (bone.z - raw.z) * 1000.0f, sqrtf(db.x * db.x + db.y * db.y + db.z * db.z) * 1000.0f, seq - lastSeq,
                (pp.x - raw.x) * 1000.0f, (pp.y - raw.y) * 1000.0f, (pp.z - raw.z) * 1000.0f, sqrtf(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z) * 1000.0f, plOk ? "" : " (no player)", controls::VehicleMode());
            {   float aw[3], lw[3], hd3[3], tg[3]; long ds = 0;
                if (arms::DiagHand(aw, lw, hd3, tg, &ds)) {
                    V3 eye{ frame.game.P.x, frame.game.P.y, frame.game.P.z };
                    float lh[3] = { NAN, NAN, NAN }; arms::DiagLiveHand(lh);
                    { float ap[10], ar[10]; arms::DiagArm(ap, ar);
                      LOG("mt3 written bones vs camera, change since last frame: R fore %.1f mm %.2f deg | L fore %.1f mm %.2f deg | R upper %.1f mm %.2f deg | L upper %.1f mm %.2f deg | R twist %.1f mm %.2f deg | L twist %.1f mm %.2f deg | helper105 %.1f mm %.2f deg | helper104 %.1f mm %.2f deg | R hand %.1f mm %.2f deg | L hand %.1f mm %.2f deg",
                          ap[0], ar[0], ap[1], ar[1], ap[2], ar[2], ap[3], ar[3], ap[4], ar[4], ap[5], ar[5], ap[6], ar[6], ap[7], ar[7], ap[8], ar[8], ap[9], ar[9]); }
                    LOG("mt2 W(apply)-W(now) %+6.1f %+6.1f %+6.1f mm | hand-eye %+7.1f %+7.1f %+7.1f mm | hand-target %+6.1f %+6.1f %+6.1f mm | apply #%ld",
                        (aw[0] - lw[0]) * 1000.0f, (aw[1] - lw[1]) * 1000.0f, (aw[2] - lw[2]) * 1000.0f,
                        (hd3[0] - eye.x) * 1000.0f, (hd3[1] - eye.y) * 1000.0f, (hd3[2] - eye.z) * 1000.0f,
                        (hd3[0] - tg[0]) * 1000.0f, (hd3[1] - tg[1]) * 1000.0f, (hd3[2] - tg[2]) * 1000.0f, ds);
                }
            }
            lastSeq = seq; lastRaw = raw; lastBone = bone; lastPl = pp;
            if (--g_motionTrace == 0) { LOG("motion trace: done"); }
        }
        const float* Wr = &F<float>(rc, cam::World);
        put(af.camRows[0], Norm({ Wr[0], Wr[1], Wr[2] })); put(af.camRows[1], Norm({ Wr[4], Wr[5], Wr[6] })); put(af.camRows[2], Norm({ Wr[8], Wr[9], Wr[10] }));
        if (af.handValid) {
            put(af.aimO, frame.ReferencePosition(ToV3(rh.aim.position)));
            put(af.aimR, frame.ReferenceDirection(QRot(rh.aim.orientation, { 1, 0, 0 })));
            put(af.aimU, frame.ReferenceDirection(QRot(rh.aim.orientation, { 0, 1, 0 })));
            put(af.aimB, frame.ReferenceDirection(QRot(rh.aim.orientation, { 0, 0, 1 })));
            put(af.gripO, frame.ReferencePosition(ToV3(rh.grip.position)));
        }
        const xr::HandState& lh = xr::Hand(0);
        af.leftValid = lh.aimValid && lh.gripValid;
        af.lSqueeze = af.leftAuthority ? lh.squeeze : 0; af.rSqueeze = af.handAuthority ? rh.squeeze : 0;
        af.machete = controls::MacheteSelected(); af.map = controls::MapOpen();
        if (af.leftValid) {
            put(af.laimO, frame.ReferencePosition(ToV3(lh.aim.position)));
            put(af.laimR, frame.ReferenceDirection(QRot(lh.aim.orientation, { 1, 0, 0 })));
            put(af.laimU, frame.ReferenceDirection(QRot(lh.aim.orientation, { 0, 1, 0 })));
            put(af.laimB, frame.ReferenceDirection(QRot(lh.aim.orientation, { 0, 0, 1 })));
            put(af.lgripO, frame.ReferencePosition(ToV3(lh.grip.position)));
        }
        {   // head (midpoint of the eyes)
            const XrPosef& hv = fv.views[0].pose;
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            put(af.headO, frame.ReferencePosition(mid)); put(af.headF, frame.ReferenceDirection(QRot(hv.orientation, { 0, 0, -1 }))); af.headValid = true;
        }
        // F2 (then 3 s) with the machete in hand: the right controller's tilt (up from level) and turn (inward from where the head
        // faces) at this moment is added to MacheteAngle / MacheteYaw and saved, so the pose you hold becomes the default
        if (g_macheteCalAt > 0) {   // countdown: a tick on the right controller each second
            static int lastTick = -1; double left = g_macheteCalAt - NowMs(); int tick = (int)ceil(left / 1000.0);
            if (left > 0 && tick != lastTick) { lastTick = tick; xr::Haptic(1, 0.3f, 0.05f); }
        }
        if (g_macheteCalAt > 0 && NowMs() >= g_macheteCalAt) {
            g_macheteCalAt = 0;
            if (af.machete && af.handAuthority) {
                V3 d = frame.ReferenceDirection(QRot(rh.aim.orientation, { 0, 0, -1 })), h = frame.ReferenceDirection(QRot(fv.views[0].pose.orientation, { 0, 0, -1 }));
                float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, d.y / fmaxf(1e-4f, sqrtf(d.x * d.x + d.y * d.y + d.z * d.z))))) * 57.29578f;
                float yaw = atan2f(h.z * d.x - h.x * d.z, h.x * d.x + h.z * d.z) * 57.29578f;   // + = turned left (inward for the right hand)
                float a0, y0, r0; arms::GetMachete(a0, y0, r0);
                float a1 = a0 + pitch, y1 = y0 + yaw;
                arms::SetMachete(a1, y1, r0);
                char path[MAX_PATH], v[32]; snprintf(path, MAX_PATH, "%sfc2vr.ini", g_dir);
                snprintf(v, sizeof v, "%.0f", a1); WritePrivateProfileStringA("VR", "MacheteAngle", v, path);
                snprintf(v, sizeof v, "%.0f", y1); WritePrivateProfileStringA("VR", "MacheteYaw", v, path);
                LOG("machete pose set: controller tilt %.1f up, %.1f inward -> MacheteAngle %.0f (was %.0f), MacheteYaw %.0f (was %.0f), saved", pitch, yaw, a1, a0, y1, y0);
                xr::Haptic(1, 0.7f, 0.15f);
            } else LOG("machete pose: ignored (machete not in the right hand)");
        }
        // two-handed grip: left grip pressed with the left hand near the weapon's foregrip (or the pistol grip)
        {
            static bool gripDown = false, grabbing = false;
            static void* gripWeapon = nullptr; static xr::InputStamp gripSource;
            bool g = lh.squeeze > (gripDown ? 0.35f : 0.6f);
            if (gripWeapon != af.weapon || gripSource.generation != af.source.generation
                || gripSource.handGeneration[0] != af.source.handGeneration[0] || gripSource.handGeneration[1] != af.source.handGeneration[1]) {
                grabbing = false; gripDown = g; gripWeapon = af.weapon; gripSource = af.source;
            }
            if (!af.handAuthority || !af.leftAuthority) grabbing = false;
            float fg[3]; bool oneH = true;
            if (g && !gripDown && af.handAuthority && af.leftAuthority && arms::Foregrip(fg, oneH)) {
                V3 d = V3{ af.lgripO[0] - fg[0], af.lgripO[1] - fg[1], af.lgripO[2] - fg[2] };
                float dist = sqrtf(Dot(d, d));
                if (dist < g_cfg.grabDistance * s) { grabbing = true; xr::Haptic(0, 0.5f, 0.05f); LOG("arms: two-handed grip (%s, %.2f m, hand span %.2f)", oneH ? "pistol" : "rifle", dist / s, arms::LastHandSpan()); }
                else LOG("arms: left grip %.2f m from the weapon (span %.2f, %s): not grabbing", dist / s, arms::LastHandSpan(), oneH ? "pistol" : "rifle");
            }
            if (!g) grabbing = false;
            gripDown = g;
            af.twoHand = grabbing;
            controls::SetLeftGripClaimed(grabbing); g_twoHandGrip = grabbing;
        }
        arms::SubmitFrame(af);
    }

}

static void __fastcall Hook_RenderViewport(void* self, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
        uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13) {
    static bool inside = false;
    void* ra = __builtin_return_address(0);
    if (g_capState == 2) { char r[96]; ModOff((uintptr_t)ra, r, sizeof r); LogWrite("capture: RenderViewport ra=%s inside=%d vp=%08x size=%08x a10(rt)=%08x a11(depth)=%08x a12=%u a13=%u", r, inside, a4 - 4, a5, a10, a11, a12, a13); }
    if (g_cfg.enable == 2) FlatDiagEnsureDevice();
    // While loading or saving the game renders two main viewports per frame (two cameras, FOV 90 and 75). Only the
    // first one of each presented frame drives the headset; a second XR frame per game frame made the view flash
    // between the two scenes.
    if (!inside && g_vpThisPresent && ra == (void*)A(eng::RenderViewportRA) && a10 == 0) {
        static int n = 0; if (n++ < 5) LOG("extra main viewport in one frame: rendered flat only");
        o_RenderViewport(self, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
        return;
    }
    if (!inside && ra == (void*)A(eng::RenderViewportRA) && a10 == 0) g_vpThisPresent = true;
    if (inside || ra != (void*)A(eng::RenderViewportRA) || g_cfg.enable != 1 || a10 != 0 || !EnsureXr()) {
        o_RenderViewport(self, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
        return;
    }
    inside = true;
    perf::SetPhase(perf::P_WAIT);
    Trace("VP>");
    double tStart = NowMs();
    g_tHookStart = tStart;
    if (g_lastFrameStart > 0) g_prof.frame += tStart - g_lastFrameStart;
    {   // adaptive detail: main-thread work of the last frame = its period minus the time spent waiting for the runtime
        static double lastWait = 0, ema = 0; static double lastLog = 0;
        if (g_lastFrameStart > 0) {
            double period = tStart - g_lastFrameStart, work = period - lastWait;
            if (period < 100.0) {
                ema = ema > 0 ? ema * 0.9 + work * 0.1 : work;
                float t = g_detailT, tgt = g_cfg.adaptiveTarget;
                if (!g_cfg.adaptiveDetail || g_cullMode == 0 || g_cullMode == 2) t = 0;
                else if (ema > tgt) t += 0.01f + 0.02f * (float)fmin(1.0, (ema - tgt) / 2.0);   // over budget: shed detail quickly
                else if (ema < tgt - 1.5) t -= 0.004f;                                           // headroom: bring it back slowly
                t = fmaxf(0.0f, fminf(g_cfg.adaptiveMax, t));
                if ((t > 0) != (g_detailT > 0) || (t > 0 && tStart - lastLog > 5000.0)) { lastLog = tStart; LOG("adaptive detail: %.2f (work %.2f ms, target %.1f)", t, ema, tgt); }
                g_detailT = t;
            }
        }
        g_detailLastWaitPtr = &lastWait;
    }
    g_lastFrameStart = tStart;
    xr::PollEvents();
    xr::FrameViews fv;
    hang::Mark("xrWaitFrame/BeginFrame");
    bool begun = xr::BeginFrame(fv);
    hang::Mark("VP: after BeginFrame");
    double tBegun = NowMs(); g_prof.wait += tBegun - tStart; if (g_detailLastWaitPtr) *g_detailLastWaitPtr = tBegun - tStart;
    perf::SetPhase(perf::P_HOOKMISC);
    perf::GpuMark(perf::G_HOOK);
    bool eyes = false; g_reflWide = false;
    if (begun) {
        static uint64_t lastSpaceGeneration = 0;
        if (lastSpaceGeneration != fv.spaceGeneration) {
            lastSpaceGeneration = fv.spaceGeneration; g_ref.valid = false; g_scopeOn = false;
            LOG("tracking: LOCAL generation %llu; auto-recenter before using new space", (unsigned long long)fv.spaceGeneration);
        }
    }
    if (begun && fv.valid && fv.positionValid) {
        // Head pose = midpoint of the eyes, orientation of the left eye (identical on HMDs).
        XrPosef head = fv.views[0].pose;
        head.position.x = 0.5f * (fv.views[0].pose.position.x + fv.views[1].pose.position.x);
        head.position.y = 0.5f * (fv.views[0].pose.position.y + fv.views[1].pose.position.y);
        head.position.z = 0.5f * (fv.views[0].pose.position.z + fv.views[1].pose.position.z);
        if (!g_ref.valid) { DoRecenter(head); g_refYTime = NowMs(); }
        Hotkeys(&head);
        {   // v222: recenter from the controllers - "Recentering 3, 2, 1" in front of the player, then the recenter
            static double rcAt = 0; static int rcShown = 0;
            if (controls::TakeRecenterRequest() && rcAt == 0) { rcAt = NowMs(); rcShown = 0; }
            if (rcAt > 0) {
                int left = 3 - (int)((NowMs() - rcAt) / 1000.0);
                if (left > 0) { if (left != rcShown) { rcShown = left; char t[48]; snprintf(t, sizeof t, "Recentering   %d", left); ShowToast(t); } }
                else { DoRecenter(head); g_refYTime = NowMs(); rcAt = 0; g_toastUntil = 0; }
            }
        }
        UpdateHeightRef(head.position.y, NowMs());
        controls::SetBodyForward(g_ref.fwdH.x, g_ref.fwdH.z, g_ref.valid);
        if ((g_frame % 900) == 0) LOG("head: y=%.3f refY=%.3f -> view offset %+.3f m", head.position.y, g_ref.pos.y, head.position.y - g_ref.pos.y + g_cfg.heightOffset);
    }
    D3D10_TEXTURE2D_DESC bbd{};
    ID3D10Texture2D* bb = begun ? AcquireBackbuffer(&bbd) : nullptr;
    bool sizeOk = g_eyeTex ? (bb != nullptr) : (bb && bbd.Width == g_xrW && bbd.Height == g_xrH);
    if (bb && !sizeOk) { static UINT warned = 0; if (warned++ % 900 == 0) LOG("backbuffer %ux%u != XR %ux%u: eye copies skipped", bbd.Width, bbd.Height, g_xrW, g_xrH); }
    if (bb) { bb->Release(); }  // the texture object stays valid for this frame; no reference is held past it
    // Zoom detection from the game camera's horizontal FOV (radians).
    if (begun && fv.valid && fv.positionValid && g_cfg.scopeMode && sizeOk) {
        float fov = F<float>((uint8_t*)(uintptr_t)(a4 - 4) + vp::RenderCam, cam::Fov);
        if (fov > 0.05f && fov < 3.0f) {
            const bool mono = controls::MonocularOut();   // v212: the game zooms all the time the monocular is out
            // v218: right after the monocular (the game still zoomed, its message box after a tag, the map back up):
            // no gun scope view
            static double monoLast = -1e9; if (mono || arms::MonoEquipped()) monoLast = NowMs();
            const bool afterMono = !mono && (arms::MonoEquipped() || controls::MapControls() || (NowMs() - monoLast < 30000.0 && !controls::NormalControls()));
            if (!g_scopeOn && !mono && !afterMono) { if (fov > g_fovBase) g_fovBase = fov; else g_fovBase += (fov - g_fovBase) * 0.0005f; }
            bool was = g_scopeOn;
            // Weapons seen zooming past the scope ratio are remembered as scoped: next time the scope view opens as soon
            // as the weapon reaches the face (no raise animation) and closes as soon as it leaves.
            static void* scoped[64]; static int nScoped = 0;
            void* wpn = arms::CurrentWeapon();
            bool known = false; for (int i = 0; i < nScoped; ++i) if (scoped[i] == wpn) known = true;
            bool sights = controls::SightsUp() && (g_twoHandGrip || !g_cfg.scopeNeedsGrip);
            if (wpn && !known && fov < g_fovBase * g_cfg.scopeFovRatio && nScoped < 64) { scoped[nScoped++] = wpn; known = true; LOG("scope: weapon %p has a scope", wpn); }
            // Weapons without a scope: the game's iron-sight pose would pull the gun to the camera (it seems to lock to
            // the body), and in VR you aim down the real sights anyway. A weapon held at the face for ScopeTestMs without
            // zooming past the scope ratio is remembered as unscoped and never gets the sight button again.
            {
                static void* plain[64]; static int nPlain = 0; static void* testW = nullptr; static double testT = 0;
                bool isPlain = false; for (int i = 0; i < nPlain; ++i) if (plain[i] == wpn) isPlain = true;
                double tn = NowMs();
                if (controls::SightsUp() && wpn && !known && !isPlain) {
                    if (testW != wpn) { testW = wpn; testT = tn; }
                    else if (tn - testT > g_cfg.scopeTestMs && nPlain < 64) { plain[nPlain++] = wpn; isPlain = true; LOG("scope: weapon %p has no scope (iron sights off for it)", wpn); }
                } else testW = nullptr;
                controls::SetSightsAllowed(!(g_cfg.ironSights == 0 && isPlain));
            }
            if (mono) g_scopeOn = controls::MonocularAtFace();   // the monocular: scope view only at the eye
            else if (afterMono) g_scopeOn = false;
            else if (g_cfg.scopeInstant && controls::Active() && known) g_scopeOn = sights;
            else if (!g_scopeOn && g_fovBase > 0 && fov < g_fovBase * g_cfg.scopeFovRatio) g_scopeOn = true;
            else if (g_scopeOn && fov > g_fovBase * (g_cfg.scopeFovRatio + 0.05f)) g_scopeOn = false;
            static float lastLogged = 0;
            if (fabsf(fov - lastLogged) > 0.02f) { lastLogged = fov; LOG("camera fov %.1f deg (normal %.1f, ratio %.2f)%s", fov * 57.2958f, g_fovBase * 57.2958f, fov / g_fovBase, g_scopeOn ? " [scope view]" : ""); }
            if (g_scopeOn && !was) {
                // Place the screen ahead of where the head looks now (yaw and pitch), fixed in the room.
                const XrPosef& h0 = fv.views[0].pose;
                V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
                V3 fwd = QRot(h0.orientation, { 0, 0, -1 });
                V3 zq = fwd * -1.0f, xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
                g_scopePose.orientation = QuatFromBasis(xq, yq, zq);
                V3 p = mid + fwd * g_cfg.scopeDistance;
                g_scopePose.position = { p.x, p.y, p.z };
            }
        }
    } else if (!g_cfg.scopeMode) g_scopeOn = false;
    if (begun && fv.valid && fv.positionValid && g_scopeOn && g_cfg.scopeFollowHead) {
        const XrPosef& h0 = fv.views[0].pose;
        V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
        V3 fwd = QRot(h0.orientation, { 0, 0, -1 });
        V3 zq = fwd * -1.0f, xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
        g_scopePose.orientation = QuatFromBasis(xq, yq, zq);
        V3 p = mid + fwd * g_cfg.scopeDistance; g_scopePose.position = { p.x, p.y, p.z };
    }
    // Snapshot the unposed native camera and recenter state BEFORE publishing arms
    // or posing scope/cull/eye cameras. Stateful smoothing advances once per frame.
    const bool poseReady = begun && fv.valid && fv.positionValid && g_ref.valid;
    float scopeO[3]{}, scopeD[3]{};
    arms::RayContext scopeContext;
    const bool monoScope = poseReady && g_scopeOn && controls::MonocularAtFace();
    const bool weaponScope = poseReady && g_scopeOn && !monoScope && g_cfg.scopeFromWeapon
        && controls::Active() && arms::SightRay(scopeO, scopeD, &scopeContext);
    if (monoScope) scopeContext = {xr::CurrentInputStamp(),arms::CurrentWeapon()};
    const bool renderScope = monoScope || weaponScope;
    const bool renderStereo = poseReady && !g_scopeOn && fv.shouldRender && g_stereoOn && sizeOk;
    // A native flat fallback still uses its native camera: keep its hands raw too.
    if (g_rsOn && poseReady) {   // v231 room-scale: the body was moved after the head - move the recenter point along
        static V3 pending[4]; static int pn = 0;
        float mv[3]; V3 m{ 0, 0, 0 };
        if (arms::TakeRoomScaleMoved(mv)) m = { mv[0], mv[1], mv[2] };
        int dl = g_rsDelay < 0 ? 0 : g_rsDelay > 3 ? 3 : g_rsDelay;
        for (int i = pn; i < dl; ++i) pending[i] = { 0, 0, 0 };
        pn = dl;
        V3 apply = m;
        if (dl > 0) { apply = pending[0]; for (int i = 0; i + 1 < dl; ++i) pending[i] = pending[i + 1]; pending[dl - 1] = m; }
        if (apply.x != 0 || apply.y != 0) {
            const GameBasis gb = BasisFromGameCam((uint8_t*)(uintptr_t)(a4 - 4) + vp::RenderCam);
            float ws = g_cfg.worldScale > 0 ? g_cfg.worldScale : 1.0f;
            float dr = (apply.x * gb.R.x + apply.y * gb.R.y) / ws, df = (apply.x * gb.F.x + apply.y * gb.F.y) / ws;
            g_ref.pos.x += g_ref.rightH.x * dr + g_ref.fwdH.x * df;
            g_ref.pos.z += g_ref.rightH.z * dr + g_ref.fwdH.z * df;
        }
    }
    const PoseFrame frame = poseReady
        ? PreparePoseFrame(g_frame, (uint8_t*)(uintptr_t)(a4 - 4) + vp::RenderCam, renderScope || renderStereo) : PoseFrame{};
    if (poseReady) {   // arms/weapon/shots follow the controllers on every frame, scoped or not
        uint8_t* rc0 = (uint8_t*)(uintptr_t)(a4 - 4) + vp::RenderCam;
        SubmitArmsFrame(fv, rc0, frame);
        {   // v232 mocap 'X' record: this frame's poses (eyes are added in the eye loop)
            float* x = cbcap::mx; memset(x, 0, sizeof cbcap::mx);
            double t = NowMs(); memcpy(x, &t, 8); uint32_t sq = (uint32_t)frame.sequence; memcpy(x + 2, &sq, 4); uint32_t fl = (uint32_t)controls::VehicleMode() | (g_riding ? 0x100u : 0u); memcpy(x + 3, &fl, 4);
            auto p3 = [](float* d, fc2vr::tracking::Vec3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; };
            p3(x + 4, frame.raw.P); p3(x + 7, frame.raw.F); p3(x + 10, frame.game.P); p3(x + 13, frame.game.F);
            float pl[3] = {}, hd = 0; arms::PlayerState(pl, &hd); memcpy(x + 16, pl, 12); x[19] = hd;
            float bp[3] = {}; arms::CamBonePos(bp); memcpy(x + 20, bp, 12); x[23] = frame.worldScale;
            auto pose = [](float* d, const XrPosef& p) { d[0] = p.position.x; d[1] = p.position.y; d[2] = p.position.z; d[3] = p.orientation.x; d[4] = p.orientation.y; d[5] = p.orientation.z; d[6] = p.orientation.w; };
            pose(x + 50, fv.views[0].pose); pose(x + 57, fv.views[1].pose);
            pose(x + 64, xr::Hand(1).grip); pose(x + 71, xr::Hand(1).aim); pose(x + 78, xr::Hand(0).grip);
            uint64_t ds = (uint64_t)fv.displayTime; memcpy(x + 86, &ds, 8);
            cbcap::mxValid = true;
        }
        if (g_rsOn) {   // v231: the head's horizontal offset from the body (recenter point), game space
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            fc2vr::tracking::Vec3 t = frame.reference.Position({ mid.x, mid.y, mid.z });
            fc2vr::tracking::Vec3 g = frame.game.Direction({ t.x, 0, t.z });
            float ws = frame.worldScale; float o3[3] = { g.x * ws, g.y * ws, 0 };
            arms::SetRoomScaleOffset(o3, true);
        }
        if (controls::FlashlightOn()) {   // v220: the head in game space for the flashlight (placed by the game thread)
            const XrPosef& h0 = fv.views[0].pose;
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            V3 f = QRot(h0.orientation, { 0, 0, -1 }), u = QRot(h0.orientation, { 0, 1, 0 });
            V3 p = frame.WorldPosition(mid + f * g_cfg.flashForward);
            V3 d = Norm(frame.WorldDirection(f));
            V3 up = Norm(frame.WorldDirection(u));
            float p3[3] = { p.x, p.y, p.z }, d3[3] = { d.x, d.y, d.z }, u3[3] = { up.x, up.y, up.z };
            arms::SetHeadWorld(p3, d3, u3);
        }
    }
    if (poseReady && g_scopeOn) {
        hang::Mark("scope view render");
        // the scope looks along the weapon's sight line (smoothed, like CrysisVR's zoomed weapon)
        uint8_t* view = (uint8_t*)(uintptr_t)(a4 - 4);
        uint8_t* rc = view + vp::RenderCam; uint8_t* cc = view + vp::CullCam;
        CamSave rsave = SaveCam(rc), csave = SaveCam(cc);
        float* ro = scopeO; float* rd = scopeD; bool posed = false;
        const bool monoRay = monoScope;
        if (monoRay) {   // v213: through the monocular the view goes where the head looks
            const XrPosef& h0 = fv.views[0].pose;
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            V3 f = QRot(h0.orientation, { 0, 0, -1 });
            V3 o = frame.ReferencePosition(mid + f * g_cfg.monoForward);   // v215: from in front of the hand holding it
            V3 d = frame.ReferenceDirection(f);
            ro[0] = o.x; ro[1] = o.y; ro[2] = o.z; rd[0] = d.x; rd[1] = d.y; rd[2] = d.z;
        }
        if (renderScope) {
            V3 pos = frame.game.Position({ ro[0], ro[1], ro[2] }), dir = Norm(frame.game.Direction({ rd[0], rd[1], rd[2] }));
            static V3 sm{ 0, 0, 0 }; static double tPrev = 0;
            static arms::RayContext previousScope;
            const bool scopeReset = previousScope.weapon != scopeContext.weapon
                || !fc2vr::authority::SameContext(previousScope.source,scopeContext.source,0)
                || !fc2vr::authority::SameContext(previousScope.source,scopeContext.source,1);
            previousScope = scopeContext;
            double tNow = NowMs(); float dt = tPrev > 0 ? (float)((tNow - tPrev) * 0.001) : 1.0f; tPrev = tNow;
            float k = scopeReset || (sm.x == 0 && sm.y == 0 && sm.z == 0) || dt > 0.5f ? 1.0f : 1.0f - expf(-dt / fmaxf(0.001f, g_cfg.scopeSmoothing));
            sm = Norm(sm * (1 - k) + dir * k);
            // the crossbow's bolt reload zooms the game's camera out and back in (to ~73 deg): the scope keeps its zoom
            static float heldFov = 0; static bool holding = false; static double holdT = 0;
            float sfov = rsave.fov;
            if (arms::ProjectileReloadActive() && heldFov > 0.01f) { if (!holding) holdT = NowMs(); holding = true; }
            else if (holding && (rsave.fov <= heldFov * 1.08f || NowMs() - holdT > 6000.0)) holding = false;   // zoomed back in (or given up)
            if (holding) sfov = heldFov; else heldFov = rsave.fov;
            float th = tanf(sfov * 0.5f), tv = th / fmaxf(0.1f, rsave.aspect);
            float gameW[16]; memcpy(gameW, &F<float>(rc, cam::World), 64);
            PoseCamera(rc, pos, sm, { 0, 0, 1 }, -th, th, -tv, tv, false);
            if (monoRay) arms::SetMonoCams(gameW, &F<float>(rc, cam::World));   // v217: the model is hidden at the eye (the mod draws the ring)
            { float o3[3] = { pos.x, pos.y, pos.z }, d3[3] = { sm.x, sm.y, sm.z }; arms::SetScopeRay(o3, d3, scopeContext); }
            PoseCamera(cc, pos, sm, { 0, 0, 1 }, -th * 1.2f, th * 1.2f, -tv * 1.2f, tv * 1.2f, false);
            posed = true;
        }
        ID3D10Texture2D* sbb = (g_cfg.scopeRound && g_cfg.uiMode == 2) ? AcquireBackbuffer() : nullptr;
        if (sbb) { g_scopeBBres = sbb; g_uiEye = 0; }
        o_RenderViewport(self, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);   // flat zoomed view (its UI goes to the HUD layer)
        if (sbb) { g_uiEye = -1; g_scopeBBres = nullptr; sbb->Release(); }
        if (g_capState == 2 && xr::HudTexture()) { SaveTexBmp(xr::HudTexture(), "cap_scopehud.bmp", 4); SaveTexBmp(xr::HudTexture(), "cap_scopehudalpha.bmp", 4); }
        if (posed) { RestoreCam(rc, rsave); RestoreCam(cc, csave); }
        g_scopeFrame = true; g_scopeFv = fv;
    } else if (renderStereo) {
        uint8_t* view = (uint8_t*)(uintptr_t)(a4 - 4);           // arg4 = &viewport.x
        uint8_t* rc = view + vp::RenderCam;
        uint8_t* cc = view + vp::CullCam;
        const GameBasis& gb = frame.game;
        const auto& ref = frame.reference;
        V3 aimGame{ F<float>(rc, cam::World + 16), F<float>(rc, cam::World + 20), F<float>(rc, cam::World + 24) };   // forward row, incl. pitch
        CamSave rsave = SaveCam(rc), csave = SaveCam(cc);
        const float s = frame.worldScale;
        { V3 ag = Norm(aimGame); g_uiAim = gb.P + ag * (g_cfg.uiDepth * s); g_uiAimValid = true; }
        // Culling camera: head pose, symmetric frustum enclosing both eyes plus margin.
        float th = 0, tv = 0;
        for (int e = 0; e < 2; ++e) {
            const XrFovf& fv2 = fv.views[e].fov;
            th = fmaxf(th, fmaxf(fabsf(tanf(fv2.angleLeft)), fabsf(tanf(fv2.angleRight))));
            tv = fmaxf(tv, fmaxf(fabsf(tanf(fv2.angleDown)), fabsf(tanf(fv2.angleUp))));
        }
        th *= g_cfg.cullMargin; tv *= g_cfg.cullMargin;
        if (g_fovMode == 2) { th *= 1.0f + g_cfg.overscanH; tv *= 1.0f + g_cfg.overscanV; }
        {
            const XrPosef& h = fv.views[0].pose;
            V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
            V3 pos = frame.WorldPosition(mid);
            V3 dir = frame.WorldDirection(QRot(h.orientation, { 0, 0, -1 }));
            V3 up  = frame.WorldDirection(QRot(h.orientation, { 0, 1, 0 }));
            g_reflWide = false;
            if (g_cullMode != 2) {
                g_reflFov = 2.0f * atanf(th); g_reflAsp = th / tv; g_reflWide = (g_cullMode == 1 || g_cullMode == 3);
                float fov0 = F<float>(cc, cam::Fov), asp0 = F<float>(cc, cam::Aspect);
                PoseCamera(cc, pos, dir, up, -th, th, -tv, tv, false);
                if (g_cullMode == 1 || g_cullMode == 3) {
                    // Keep the wide VR frustum planes, but give everything that reads the camera's FOV/projection
                    // (LOD / screen-size culling) the game's own flat values.
                    // Mode 3 keeps only the scalar FOV/aspect flat: the matrices stay wide, because the water's planar
                    // reflection is rendered through this camera's projection and a flat one leaves the edges of
                    // the VR view without reflections (streaks at the sides, reflections vanishing up/down).
                    uint8_t planes[cam::InDir - cam::Frustum]; memcpy(planes, cc + cam::Frustum, sizeof planes);
                    uint8_t mats[cam::Pos - cam::View]; memcpy(mats, cc + cam::View, sizeof mats);
                    if (g_detailT > 0) {   // adaptive detail: part of the way from the flat FOV toward the VR one
                        float tf = tanf(fov0 * 0.5f), tw = th, t = g_detailT;
                        float tb = tf + (tw - tf) * t; fov0 = 2.0f * atanf(tb);
                        asp0 = asp0 + (th / tv - asp0) * t;
                    }
                    F<float>(cc, cam::Fov) = fov0; F<float>(cc, cam::Aspect) = asp0;
                    ((UpdateMatrices_t)A(eng::UpdateMatrices))(cc);
                    memcpy(cc + cam::Frustum, planes, sizeof planes);
                    if (g_cullMode == 3) memcpy(cc + cam::View, mats, sizeof mats);
                }
            }
        }
        double tCams = NowMs(); g_prof.cams += tCams - tBegun;
        if (g_capState == 2) {
            const float* P = &F<float>(cc, cam::Proj);
            LOG("capture: cull cam fov=%.4f aspect=%.4f proj %.3f %.3f %.3f %.3f | render cam fov=%.4f aspect=%.4f | cullMode %d fovMode %d",
                F<float>(cc, cam::Fov), F<float>(cc, cam::Aspect), P[0], P[5], P[8], P[9], F<float>(rc, cam::Fov), F<float>(rc, cam::Aspect), g_cullMode, g_fovMode);
        }
        // Symmetric eye frustums: the game's screen-space effects (water reflections) assume the projection is centred
        // on the screen. Each eye renders the symmetric frustum enclosing its real one, and the compositor is told so.
        // Eye frustums. The game's screen-space water reflections assume a projection centred on the screen and can only
        // reflect what is inside the rendered image. Mode 1 renders the symmetric frustum enclosing each eye's real one;
        // mode 2 also adds a hidden border (overscan). Only the part covering the eye's real FOV is shown in the headset
        // (sub-image rect), so the border stays invisible but the reflections near the edges have something to sample.
        const XrFovf realFov0 = fv.views[0].fov;
        float rtan[2][4];   // render tangents L R D U
        for (int e = 0; e < 2; ++e) {
            const XrFovf& f = fv.views[e].fov;
            float tl = tanf(f.angleLeft), tr = tanf(f.angleRight), td = tanf(f.angleDown), tu = tanf(f.angleUp);
            if (g_fovMode == 0 || !g_eyeTex) { rtan[e][0] = tl; rtan[e][1] = tr; rtan[e][2] = td; rtan[e][3] = tu; continue; }
            float H = fmaxf(fabsf(tl), fabsf(tr)), V = fmaxf(fabsf(td), fabsf(tu));
            if (g_fovMode == 2) { H *= 1.0f + g_cfg.overscanH; V *= 1.0f + g_cfg.overscanV; }
            rtan[e][0] = -H; rtan[e][1] = H; rtan[e][2] = -V; rtan[e][3] = V;
            // pixel rect of the real FOV inside the rendered image (y down = tangent up first)
            const float W = (float)(g_upscale ? g_outSize[0] : g_eyeSize[0]), Hp = (float)(g_upscale ? g_outSize[1] : g_eyeSize[1]);   // swapchain pixels
            int x0 = (int)floorf((tl + H) / (2 * H) * W), x1 = (int)ceilf((tr + H) / (2 * H) * W);
            int y0 = (int)floorf((V - tu) / (2 * V) * Hp), y1 = (int)ceilf((V - td) / (2 * V) * Hp);
            x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > (int)W ? (int)W : x1; y1 = y1 > (int)Hp ? (int)Hp : y1;
            xr::SetEyeRect(e, x0, y0, x1 - x0, y1 - y0);
            // the FOV submitted is exactly what that whole-pixel rect covers
            XrFovf& o = fv.views[e].fov;
            o.angleLeft = atanf(x0 / W * 2 * H - H); o.angleRight = atanf(x1 / W * 2 * H - H);
            o.angleUp = atanf(V - y0 / Hp * 2 * V); o.angleDown = atanf(V - y1 / Hp * 2 * V);
            static int logged = 0;
            if (logged < 2 && g_fovMode == 2) { ++logged; LOG("eye %d: real tan L%.3f R%.3f D%.3f U%.3f, render +-%.3f/+-%.3f, shown rect %d,%d %dx%d of %dx%d",
                e, tl, tr, td, tu, H, V, x0, y0, x1 - x0, y1 - y0, (int)W, (int)Hp); }
        }
        // DLSS: this frame's sub-pixel jitter (render pixels; 0 when DLSS is off), and each eye's unjittered
        // view-projection of this and the previous frame for the motion vectors.
        float jx = 0, jy = 0;
        static double s_vpPrev[2][16]; static V3 s_posPrev[2]; static uint32_t s_prevFrame[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
        {   // a sudden turn of the view (snap turn, a cut): no history for DLSS, no AFW rebuild from the old view
            static V3 lastF{}; static bool haveF = false;
            V3 fN = Norm(gb.F);
            if (haveF && Dot(fN, lastF) < 0.94f) {   // more than ~20 degrees in one frame
                s_prevFrame[0] = s_prevFrame[1] = 0xFFFFFFFFu; afw::Invalidate();
                static int nl = 0; if (nl++ < 5) LOG("view turned %.0f deg in one frame - DLSS history and AFW sources reset", acosf(fmaxf(-1.0f, fminf(1.0f, Dot(fN, lastF)))) * 57.2958f);
            }
            lastF = fN; haveF = true;
        }
        // AFW (experimental): one eye rendered per frame, alternating; the other rebuilt from its last image
        bool afwOn = afw::Wanted() && g_eyeTex && g_eyeLinTex && !g_scopeFrame && !g_upscale;
        bool afwSkip[2] = { false, false }; double afwTarget[2][16];
        if (afwOn) {
            static V3 lastCam{}; static bool haveCam = false;
            V3 dc = gb.P - lastCam;
            if (haveCam && Dot(dc, dc) > 9.0f * s * s) { afw::Invalidate(); LOG("afw: camera jump of %.1f m - both eyes rendered", sqrtf(Dot(dc, dc)) / s); }
            lastCam = gb.P; haveCam = true;
            int fresh = (int)(g_frame & 1), held = 1 - fresh;
            if (afw::HasSource(held, g_frame)) afwSkip[held] = true;
        }
        g_uiPrimary = afwSkip[0] ? 1 : 0;   // the UI is drawn with the first eye rendered this frame
        bool dlssOn = dlssfc2::Wanted() && g_eyeTex && g_eyeDepthTex && !g_scopeFrame;
        xr::SetCopyBack(afwOn && dlssOn);   // AFW + DLSS: each eye is rebuilt from its own last DLSS result
        if (dlssOn) dlssfc2::BeginFrame(&jx, &jy);
        for (int e = 0; e < 2; ++e) {
            double tE0 = NowMs();
            const XrView& v = fv.views[e];
            V3 pos = frame.WorldPosition(ToV3(v.pose.position));
            V3 dir = frame.WorldDirection(QRot(v.pose.orientation, { 0, 0, -1 }));
            V3 up  = frame.WorldDirection(QRot(v.pose.orientation, { 0, 1, 0 }));
            PoseCamera(rc, pos, dir, up, rtan[e][0], rtan[e][1], rtan[e][2], rtan[e][3], true);
            { float* x = cbcap::mx + 32 + e * 9; x[0] = pos.x; x[1] = pos.y; x[2] = pos.z; x[3] = dir.x; x[4] = dir.y; x[5] = dir.z; x[6] = up.x; x[7] = up.y; x[8] = up.z; }
            double vpCur[16]; bool havePrev = false;
            { const float* VP = &F<float>(rc, cam::ViewProj); for (int i = 0; i < 16; ++i) vpCur[i] = VP[i]; }
            if (afwSkip[e]) { memcpy(afwTarget[e], vpCur, sizeof vpCur); continue; }   // rebuilt after the loop
            if (dlssOn) {
                V3 dp = pos - s_posPrev[e];
                // the eye's previous image: last frame, or two frames ago under AFW (each eye renders every other frame)
                havePrev = s_prevFrame[e] != 0xFFFFFFFFu && g_frame - s_prevFrame[e] <= (afwOn ? 2u : 1u) && Dot(dp, dp) < 4.0f * s * s;
                if (jx != 0 || jy != 0) {   // the same frustum shifted by the jitter: pixel (x, y) samples (x + 0.5 + jx, y + 0.5 + jy)
                    const float W = (float)g_eyeSize[0], H = (float)g_eyeSize[1];
                    float dx = jx * (rtan[e][1] - rtan[e][0]) / W, dy = -jy * (rtan[e][3] - rtan[e][2]) / H;
                    PoseCamera(rc, pos, dir, up, rtan[e][0] + dx, rtan[e][1] + dx, rtan[e][2] + dy, rtan[e][3] + dy, true);
                }
            }
            perf::SetPhase(e ? perf::P_EYE1 : perf::P_EYE0);
            static bool s_census = true;
            if (e == 0 && (dlssOn || afw::Wanted()) && s_census) { g_eyeBindsOn = true; }
            g_inEye = true; g_uiEye = e; g_renderEye = e; g_capEye = e; hang::Mark(e ? "eye1 render" : "eye0 render");
            if (g_capState == 2) TraceMark(e ? "eye1 dispatch begin" : "eye0 dispatch begin");
            if (g_eyeTex) {
                // Off-screen at headset resolution, exactly like the engine's tiled-screenshot path:
                // position {0,0}, explicit size, our colour + depth targets.
                o_RenderViewport(self, edx, a1, a2, a3, (uint32_t)(uintptr_t)g_eyePos, (uint32_t)(uintptr_t)g_eyeSize,
                                 a6, a7, a8, a9, (uint32_t)(uintptr_t)g_eyeRT, (uint32_t)(uintptr_t)g_eyeDepth, a12, a13);
                g_inEye = false; g_uiEye = -1; g_renderEye = -1;
                if (g_capState == 2 && e == 0) SaveTexBmp(g_eyeTex, "cap_eyeraw.bmp", 4);
                if (g_uiIsolated[e]) { g_dev10->CopyResource(g_eyeTex, g_preUi); g_uiIsolated[e] = false; }   // UI-free eye image
                if (g_eyeBindsOn) {
                    g_eyeBindsOn = false; s_census = false;
                    ID3D10Texture2D* best = nullptr; int bestN = 0;
                    for (int i = 0; i < g_nEyeBinds; ++i) {
                        D3D10_TEXTURE2D_DESC td; g_eyeBinds[i].t->GetDesc(&td);
                        LOG("dlss census: eye render binds %s %p fmt %d bind 0x%x %d times%s", g_eyeBinds[i].depth ? "depth" : "colour", (void*)g_eyeBinds[i].t,
                            (int)td.Format, td.BindFlags, g_eyeBinds[i].n, g_eyeBinds[i].t == g_eyeDepthTex ? " (the eye depth target)" : g_eyeBinds[i].t == g_eyeTex ? " (the eye colour target)" : "");
                        if (g_eyeBinds[i].depth && g_eyeBinds[i].n > bestN) { bestN = g_eyeBinds[i].n; best = g_eyeBinds[i].t; }
                    }
                    {   // the engine's linear depth (R32F colour target, "Linear depth" pass): the depth buffer itself ends up cleared
                        ID3D10Texture2D* lin = nullptr; int ln = 0;
                        for (int i = 0; i < g_nEyeBinds; ++i) if (!g_eyeBinds[i].depth) { D3D10_TEXTURE2D_DESC td; g_eyeBinds[i].t->GetDesc(&td);
                            if ((td.Format == DXGI_FORMAT_R32_FLOAT || td.Format == DXGI_FORMAT_R16_FLOAT) && (td.BindFlags & D3D10_BIND_SHADER_RESOURCE) && g_eyeBinds[i].n > ln) { ln = g_eyeBinds[i].n; lin = g_eyeBinds[i].t; } }
                        if (lin) { LOG("dlss: linear depth candidate %p", (void*)lin); dlssfc2::SetLinearDepth(lin); lin->AddRef(); if (g_eyeLinTex) g_eyeLinTex->Release(); g_eyeLinTex = lin; }
                    }
                    if (best && best != g_eyeDepthTex) { LOG("dlss: using the depth buffer the eye render binds most (%p) instead of %p", (void*)best, (void*)g_eyeDepthTex); best->AddRef(); if (g_eyeDepthTex) g_eyeDepthTex->Release(); g_eyeDepthTex = best; }
                    for (int i = 0; i < g_nEyeBinds; ++i) g_eyeBinds[i].t->Release();
                    g_nEyeBinds = 0;
                }
                if (afwOn) { float gr[2][6]; GripsGame(frame, gr); afw::Store(g_dev10, e, g_eyeTex, g_eyeLinTex, vpCur, F<float>(rc, cam::Near), F<float>(rc, cam::Far), g_frame, gr); }
                if (dlssOn) {
                    dlssfc2::PrepareEye10(g_dev10, e, g_eyeDepthTex, vpCur, havePrev ? s_vpPrev[e] : nullptr, g_nearMaxDepth, jx, jy, F<float>(rc, cam::Near), F<float>(rc, cam::Far), rtan[e]);
                    memcpy(s_vpPrev[e], vpCur, sizeof vpCur); s_posPrev[e] = pos; s_prevFrame[e] = g_frame;
                }
                if (g_capState == 2) TraceMark(e ? "eye1 dispatch end" : "eye0 dispatch end");
                if (g_capState == 2 && e == 0) { SaveTexBmp(g_eyeTex, "cap_eye0.bmp", 4); if (xr::HudTexture()) { SaveTexBmp(xr::HudTexture(), "cap_hud.bmp", 4); SaveTexBmp(xr::HudTexture(), "cap_hudalpha.bmp", 4); } if (xr::ReticleTexture()) { SaveTexBmp(xr::ReticleTexture(), "cap_ret.bmp", 4); SaveTexBmp(xr::ReticleTexture(), "cap_retalpha.bmp", 4); }
                }
                if (e == 0 && g_snapAt > 0 && NowMs() >= g_snapAt) {   // v228 test: where the game's messages are drawn
                    g_snapAt = 0; g_snapLast = NowMs(); ++g_snaps;
                    char nm[48]; snprintf(nm, 48, "snap%d_eye0.bmp", g_snaps); SaveTexBmp(g_eyeTex, nm, 5);
                    if (xr::HudTexture()) { snprintf(nm, 48, "snap%d_hud.bmp", g_snaps); SaveTexBmp(xr::HudTexture(), nm, 5); snprintf(nm, 48, "snap%d_hudalpha.bmp", g_snaps); SaveTexBmp(xr::HudTexture(), nm, 5); }
                    LOG("HUD snapshot %d (action map %s)", g_snaps, controls::NormalControls() ? "default" : controls::LoadingControls() ? "locked" : "other");
                }
                perf::GpuMark(e ? perf::G_EYE1 : perf::G_EYE0);
                perf::SetPhase(perf::P_HOOKMISC);
                double tR = NowMs(); (e ? g_prof.eye1 : g_prof.eye0) += tR - tE0;
                xr::CopyEye(e, g_eyeTex);
                g_prof.copies += NowMs() - tR;
            } else {
                o_RenderViewport(self, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
                ID3D10Texture2D* cur = AcquireBackbuffer();
                if (cur) { xr::CopyEye(e, cur); cur->Release(); }
            }
        }
        for (int e = 0; e < 2; ++e) if (afwSkip[e]) {   // the held eye: rebuilt from its own last image and the fresh eye's
            double tA = NowMs();
            ID3D10Texture2D* heldColor = nullptr;
            if (dlssOn && dlssfc2::Active()) { xr::WaitEyeCopies(); heldColor = xr::EyeTexture10(e); }   // its last DLSS result
            float gr[2][6]; GripsGame(frame, gr);
            ID3D10Texture2D* t = afw::Rebuild(g_dev10, e, 1 - e, afwTarget[e], heldColor, gr);
            if (t) xr::CopyEye(e, t);   // else the eye keeps its last image
            (e ? g_prof.eye1 : g_prof.eye0) += NowMs() - tA;
        }
        g_eyeCam = nullptr; g_uiPrimary = 0;
        {   // v228: the lowered top strip only in gameplay - not the main menu, loading screens or the pause menu
            static bool inGame = false; static double normSince = -1; float pp[3], hd = 0; const double tn = NowMs();
            const bool loadPic = tn - g_loadPicMs < 3000.0;
            if (!controls::NormalControls()) normSince = -1; else if (normSince < 0) normSince = tn;
            // quitting to the main menu passes the pause menu; a loading screen shows its picture and the default controls
            // come up ~1 s before it fades - so gameplay only after 2.5 s of default controls
            if (!arms::PlayerState(pp, &hd) || controls::MenuControls() || loadPic) inGame = false;
            else if (normSince >= 0 && tn - normSince > 2500.0) inGame = true;
            g_snapGame = inGame && !controls::MenuControls();
            xr::SetHudTopBandOn(g_snapGame);
        }
        if (g_cfg.uiMode == 2 && g_uiLayerFresh) {
            g_uiLayerFresh = false;
            const XrFovf& f0 = realFov0;
            float D = g_cfg.hudDistance;
            float w = D * (tanf(f0.angleRight) - tanf(f0.angleLeft)) * g_cfg.uiScale * g_cfg.hudScale;
            if (controls::MenuControls()) w *= g_cfg.pauseMenuScale;   // the in-game pause menu panel
            XrPosef hp; hp.orientation = { 0, 0, 0, 1 }; hp.position = { 0, g_cfg.uiHeadY, -D };
            if (!(controls::MonocularOut() && !controls::MonocularAtFace())) xr::SetHudLayer(hp, w, true);   // v213: the monocular's marker icons are placed for the game's zoomed camera - shown only through it
        }
        HudTick();
        // aiming dot: only where the game itself would show a reticle (none for the machete, flamethrower...);
        // on the weapon's sight line (CrysisVR: 10 m), or straight ahead for guns not held in the hands (turrets)
        bool wantDot = g_cfg.reticleDot && controls::Active() && !controls::MacheteSelected() && !controls::MapOpen() && (!g_hudOk || g_gameReticle)
                       && arms::CurrentWeapon() != nullptr;   // empty hands (weapon away, climbing): no dot - the game keeps its reticle flag
        if (wantDot) {
            float ro[3], rd[3];
            bool handRay = controls::MountedWeapon() ? arms::CameraRay(ro, rd) : arms::SightRay(ro, rd);   // a turret aims along the game camera
            if (!handRay && g_hudOk) {   // no gun in the hands: the view's forward direction
                const XrPosef& h0 = fv.views[0].pose;
                V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
                V3 fwd = QRot(h0.orientation, { 0, 0, -1 });
                V3 p = mid + fwd * g_cfg.reticleDistance;
                V3 zq = fwd * -1.0f, xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
                XrPosef dp; dp.orientation = QuatFromBasis(xq, yq, zq); dp.position = { p.x, p.y, p.z };
                xr::SetDotLayer(dp, g_cfg.reticleSize);
            }
            if (handRay) {
                float ad = controls::MountedWeapon() ? 0.0f : arms::AimDistance();   // on what the weapon points at (no parallax), else ReticleDistance
                const float D = ad > 0.0f ? ad : g_cfg.reticleDistance * s;
                V3 pr{ ro[0] + rd[0] * D, ro[1] + rd[1] * D, ro[2] + rd[2] * D };   // ref frame, game units
                V3 p = frame.LocalPosition(pr);
                V3 mid = (ToV3(fv.views[0].pose.position) + ToV3(fv.views[1].pose.position)) * 0.5f;
                V3 zq = Norm(mid - p);                                 // face the viewer
                V3 xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
                XrPosef dp; dp.orientation = QuatFromBasis(xq, yq, zq); dp.position = { p.x, p.y, p.z };
                float dist = sqrtf(Dot(mid - p, mid - p));
                xr::SetDotLayer(dp, g_cfg.reticleSize * dist / fmaxf(0.01f, g_cfg.reticleDistance));
            }
        }
        {   // throwing arc (grip + trigger): drawn into both eyes
            static float ap[64][3]; bool land = false, aok = false; float an[3];
            int n = arms::GetThrowArc(ap, 64, land, an, aok);
            if (n >= 2) {
                XrVector3f lp[64];
                for (int i = 0; i < n; ++i) {
                    V3 v = frame.LocalPosition({ ap[i][0], ap[i][1], ap[i][2] });
                    lp[i] = { v.x, v.y, v.z };
                }
                V3 nn = frame.LocalDirection({ an[0], an[1], an[2] });
                xr::SetArc(lp, n, land, XrVector3f{ nn.x, nn.y, nn.z }, aok);
            }
        }
        if (g_cfg.uiMode == 2 && g_uiRetFresh) {
            g_uiRetFresh = false;
            const XrFovf& f0 = realFov0;
            V3 ag = Norm(aimGame);
            V3 r{ Dot(ag, gb.R), Dot(ag, gb.U), -Dot(ag, gb.F) };
            V3 aim = Norm(frame.LocalDirection(r));
            V3 zq = aim * -1.0f;
            V3 xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
            XrPosef rp; rp.orientation = QuatFromBasis(xq, yq, zq);
            float D = g_cfg.uiDepth;
            rp.position = { ref.origin.x + aim.x * D, ref.origin.y - ref.heightOffset + aim.y * D, ref.origin.z + aim.z * D };
            float frac = 0.3f;
            xr::SetReticleLayer(rp, D * (tanf(f0.angleRight) - tanf(f0.angleLeft)) * g_cfg.uiScale * frac, frac);
        }
        // HUD quad: centred on the gun's aim ray (game camera forward incl. pitch) from the recentered head point.
        if (g_cfg.hudLayer && g_hudRTV && g_hudFresh) {
            g_hudFresh = false;
            V3 ag = Norm(aimGame);
            V3 r{ Dot(ag, gb.R), Dot(ag, gb.U), -Dot(ag, gb.F) };                     // game -> reference frame
            V3 aim = Norm(frame.LocalDirection(r)); // reference -> LOCAL
            V3 zq = aim * -1.0f;                                                           // quad normal faces the viewer
            V3 xq = Norm(Cross(V3{ 0, 1, 0 }, zq)), yq = Cross(zq, xq);
            XrPosef hp; hp.orientation = QuatFromBasis(xq, yq, zq);
            float D = g_cfg.hudDistance;
            hp.position = { ref.origin.x + aim.x * D, ref.origin.y + aim.y * D, ref.origin.z + aim.z * D };
            if (!(controls::MonocularOut() && !controls::MonocularAtFace())) xr::SetHudLayer(hp, 2.0f * D * tanf(g_cfg.hudFov * 0.5f * 3.14159265f / 180.0f));
        }
        RestoreCam(rc, rsave);
        RestoreCam(cc, csave);
        if (g_eyeTex) {   // desktop mirror: centred crop of the right eye (the HUD is drawn on top afterwards)
            D3D10_TEXTURE2D_DESC ed, bd; g_eyeTex->GetDesc(&ed);
            ID3D10Texture2D* cur = AcquireBackbuffer(&bd);
            if (cur) {
                UINT cw = ed.Width < bd.Width ? ed.Width : bd.Width, ch = ed.Height < bd.Height ? ed.Height : bd.Height;
                D3D10_BOX box{ (ed.Width - cw) / 2, (ed.Height - ch) / 2, 0, (ed.Width - cw) / 2 + cw, (ed.Height - ch) / 2 + ch, 1 };
                if (bd.SampleDesc.Count == 1) g_dev10->CopySubresourceRegion(cur, 0, (bd.Width - cw) / 2, (bd.Height - ch) / 2, 0, g_eyeTex, 0, &box);
                cur->Release();
            }
        }
        eyes = true;
    } else {
        o_RenderViewport(self, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    }
    if (poseReady && g_frame % 90 == 0) {
        arms::FrameUsage use;
        if (arms::ReadFrameUsage(use)) LOG("pose use shared-camera-1 published=%llu eyes=%d scope=%d renderThread=%lu appliedValid=%d applied=%llu appliedCalls=%u applyThread=%u interactionValid=%d interaction=%llu interactionCalls=%u interactionThread=%u",
            (unsigned long long)frame.sequence, eyes, g_scopeFrame, GetCurrentThreadId(), use.haveApplied,
            (unsigned long long)use.appliedSequence, use.appliedCalls, use.appliedThread, use.haveInteraction,
            (unsigned long long)use.interactionSequence, use.interactionCalls, use.interactionThread);
    }
    double tEnd0 = NowMs();
    // A VP frame without eyes (loading, save thumbnails, ...) would otherwise be an empty (black) frame: show the flat
    // screen instead, ended at Present once the backbuffer is complete.
    if (begun && !eyes && !g_scopeFrame) { g_screenFrame = true; g_screenFv = fv; }
    hang::Mark("xrEndFrame"); if (begun && !g_scopeFrame && !g_screenFrame) xr::EndFrame(fv, eyes); hang::Mark("VP: after EndFrame");   // scope/screen frames end at Present
    g_vpEyes = eyes;
    g_prof.end += NowMs() - tEnd0;
    if (eyes) { g_profArmed = true; if (++g_prof.n >= 90) ProfReport(); }
    g_eyesThisFrame = begun;
    g_tHookEnd = NowMs();
    perf::SetPhase(perf::P_RFPOST);
    Trace("VP<");
    if ((++g_frame % 900) == 1) { LOG("frame %u: xr running=%d eyes=%d", g_frame, xr::SessionRunning(), eyes); LogVA("frame"); }
    inside = false;
}

static XrPosef MenuPose() {
    XrPosef qp{}; qp.orientation.w = 1.0f;
    qp.position = { 0.0f, 0.0f, -g_cfg.menuDistance };
    if (g_ref.valid) {
        V3 f = g_ref.fwdH; float dist = g_cfg.menuDistance;
        qp.position = { g_ref.pos.x + f.x * dist, g_ref.pos.y, g_ref.pos.z + f.z * dist };
        float a = atan2f(-f.x, -f.z);
        qp.orientation = { 0.0f, sinf(a * 0.5f), 0.0f, cosf(a * 0.5f) };
    }
    return qp;
}
static void __fastcall Hook_Present(void* self, void* edx, int a1) {
    hang::Beat(); hang::Mark("present");
    cbcap::Present(g_dir);
    {
        static int logged = 0; static uintptr_t lastSc = 0; static int changes = 0;
        uint8_t* drv = (uint8_t*)self;
        uintptr_t sc = *(uintptr_t*)(drv + 0x40), cur = *(uintptr_t*)(drv + 0x78);
        if (sc != lastSc) ++changes;
        if (logged < 12 || (sc != lastSc && changes % 50 == 0)) {
            ++logged;
            char cls[64] = "", cls2[64] = "", title[64] = "";
            if (a1) { GetClassNameA((HWND)(uintptr_t)a1, cls, 64); GetWindowTextA((HWND)(uintptr_t)a1, title, 64); }
            if (cur) GetClassNameA((HWND)cur, cls2, 64);
            RECT rc{}; if (a1) GetClientRect((HWND)(uintptr_t)a1, &rc);
            LogWrite("PRESENT target=%08x(%s '%s' client %ldx%ld) stored=%08x(%s) swapchain=%08x changes=%d",
                     (unsigned)a1, cls, title, rc.right, rc.bottom, (unsigned)cur, cls2, (unsigned)sc, changes);
        }
        lastSc = sc;
        // v226: the game's window must keep the client size it asked for (the backbuffer). Maximized or stretched, the
        // game's size check fails every frame and fires its window-resized event: every tree's render data is rebuilt
        // each frame (~6 ms) and the quality settings are applied again (shadow map size reset). Put the window back.
        static int bad = 0, fixes = 0; static ULONGLONG nextChk = 0; ULONGLONG nowT = GetTickCount64();
        if (a1 && nowT >= nextChk) {
            nextChk = nowT + 1000;
            HWND hw = (HWND)(uintptr_t)a1; RECT rc{}; GetClientRect(hw, &rc);
            int bw = *(int*)(drv + 0x4C), bh = *(int*)(drv + 0x50);
            bool sane = bw >= 320 && bh >= 200 && bw <= 16384 && bh <= 16384;
            if (sane && (rc.right != bw || rc.bottom != bh) && !IsIconic(hw)) {
                if (++bad >= 3 && fixes < 10) {
                    ++fixes; bad = 0;
                    if (IsZoomed(hw)) ShowWindowAsync(hw, SW_RESTORE);
                    RECT w{ 0, 0, bw, bh }; DWORD st = (DWORD)GetWindowLongA(hw, GWL_STYLE), ex = (DWORD)GetWindowLongA(hw, GWL_EXSTYLE);
                    AdjustWindowRectEx(&w, st, FALSE, ex);
                    SetWindowPos(hw, nullptr, 0, 0, w.right - w.left, w.bottom - w.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    LOG("window: client %ldx%ld but the game renders %dx%d (%s) - window put back to the game's size", rc.right, rc.bottom, bw, bh, IsZoomed(hw) ? "maximized" : "resized");
                }
            } else bad = 0;
        }
    }
    // Frames without a 3D gameplay view (menus, loading, map screens) are shown as a virtual screen.
    if (g_scopeFrame) {
        g_scopeFrame = false;
        ID3D10Texture2D* bb = AcquireBackbuffer();
        if (g_cfg.scopeRound && g_cfg.uiMode == 2) {
            // round window over a square centre crop; the game's own scope reticle (its UI, centre crop) on top;
            // the ammo counter (lower right of the game screen) inside the window, lower right
            float side = (float)(g_scrW < g_scrH ? g_scrW : g_scrH);
            float rc[4] = { (g_scrW - side) * 0.5f, (g_scrH - side) * 0.5f, side, side };
            float ra[4] = { g_scrW * g_cfg.scopeHudX, g_scrH * g_cfg.scopeHudY, g_scrW * (1.0f - g_cfg.scopeHudX), g_scrH * (1.0f - g_cfg.scopeHudY) };
            float W = g_cfg.scopeWidth * g_cfg.scopeRoundSize;
            V3 xr_ = QRot(g_scopePose.orientation, { 1, 0, 0 }), yr_ = QRot(g_scopePose.orientation, { 0, 1, 0 }), zr_ = QRot(g_scopePose.orientation, { 0, 0, 1 });
            float hw = W * g_cfg.scopeAmmoSize;
            V3 c = ToV3(g_scopePose.position) + xr_ * (W * g_cfg.scopeAmmoX) + yr_ * (W * g_cfg.scopeAmmoY) + zr_ * 0.002f;
            XrPosef hp = g_scopePose; hp.position = { c.x, c.y, c.z };
            ID3D10Texture2D* src = bb; bool resolved = false;
            bool own = g_cfg.scopeAmmoOwn && g_hudAmmoSeq > 0;
            if (own) { static LONG drawn = -1; if (drawn != g_hudAmmoSeq) { drawn = g_hudAmmoSeq; DrawAmmo(g_hudClip, g_hudPouch); } }
            if (!own && g_scopePreValid && bb && g_scopePre && g_scopeComp) {
                D3D10_TEXTURE2D_DESC bd, pd; bb->GetDesc(&bd); g_scopePre->GetDesc(&pd);
                ID3D10Texture2D* from = bb;
                if (bd.SampleDesc.Count > 1) {
                    if (!g_scopeBbTmp) { D3D10_TEXTURE2D_DESC td = bd; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0; td.BindFlags = D3D10_BIND_SHADER_RESOURCE; td.MiscFlags = 0; td.CPUAccessFlags = 0; td.Usage = D3D10_USAGE_DEFAULT;
                        if (FAILED(g_dev10->CreateTexture2D(&td, nullptr, &g_scopeBbTmp))) g_scopeBbTmp = nullptr; }
                    if (g_scopeBbTmp) g_dev10->ResolveSubresource(g_scopeBbTmp, 0, bb, 0, bd.Format);
                    from = g_scopeBbTmp;
                }
                if (from && bd.Width == pd.Width && bd.Height == pd.Height && TypelessFamily10(bd.Format) == TypelessFamily10(pd.Format)) {
                    g_dev10->CopyResource(g_scopeComp, g_scopePre);
                    D3D10_BOX box{ (UINT)ra[0], (UINT)ra[1], 0, (UINT)(ra[0] + ra[2]), (UINT)(ra[1] + ra[3]), 1 };
                    if (box.right > bd.Width) box.right = bd.Width; if (box.bottom > bd.Height) box.bottom = bd.Height;
                    if (box.right > box.left && box.bottom > box.top) ScopeAmmoKey(from, box);
                    src = g_scopeComp; resolved = true;
                } else { static bool warned = false; if (!warned) { warned = true; LOG("scope: kept scene not usable (bb %ux%u fmt %d, scene fmt %d)", bd.Width, bd.Height, (int)bd.Format, (int)pd.Format); } }
            }
            g_scopePreValid = false;
            const bool monoView = controls::MonocularOut();   // v214: the monocular's own window (its targeting ring, no crosshair / ammo)
            if (monoView && g_scopePre) { src = g_scopePre; resolved = true; }   // v215: the scene before the UI (no tutorial / button prompts); the ring is the monocular's own lens
            xr::SetMonoTarget(monoView && arms::MonoTarget());
            {   static double nextDiag = 0; static int nDiag = 0;   // v231: is the kept scene black before it reaches the scope layer?
                if (monoView && nDiag < 8 && NowMs() > nextDiag) { nextDiag = NowMs() + 3000.0; ++nDiag;
                    D3D10_TEXTURE2D_DESC sd{}; if (src) src->GetDesc(&sd);
                    LOG("monocular view: scene %p %ux%u fmt %d, centre brightness %.4f (backbuffer %.4f), dlss upscale %d", (void*)src, sd.Width, sd.Height, (int)sd.Format, CentreLuma(src), CentreLuma(bb), g_upscale ? 1 : 0); } }
            xr::EndFrameScope(g_scopeFv, src, g_scopePose, W, (g_cfg.scopeGameReticle && !monoView) ? rc : nullptr, ra, hp, hw, resolved, own && !monoView, monoView);
        } else xr::EndFrameScreen(g_scopeFv, bb, g_scopePose, g_cfg.scopeWidth);
        if (bb) bb->Release();
        PathNote('Z');
    } else if (g_screenFrame) {
        g_screenFrame = false;
        D3D10_TEXTURE2D_DESC d{};
        ID3D10Texture2D* bb = AcquireBackbuffer(&d);
        XrPosef qp = MenuPose();
        xr::EndFrameScreen(g_screenFv, (bb && d.Width == g_scrW && d.Height == g_scrH) ? bb : nullptr, qp, g_cfg.menuWidth);
        if (bb) bb->Release();
        PathNote('Q');
    } else if (g_eyesThisFrame && g_vpEyes) {
        PathNote('E');
    } else if (g_cfg.enable && xr::Ready()) {
        xr::PollEvents();
        if (!g_eyesThisFrame) {
            D3D10_TEXTURE2D_DESC d;
            ID3D10Texture2D* bb = AcquireBackbuffer(&d);
            if (bb) {
                if (d.Width == g_scrW && d.Height == g_scrH) {
                    // Menus/loading: a flat screen MenuDistance ahead of the recentered facing direction, at head height.
                    XrPosef qp{}; qp.orientation.w = 1.0f;
                    const XrPosef* pp = &qp;
                    qp.position = { 0.0f, 0.0f, -g_cfg.menuDistance };   // before the first recenter: ahead of the runtime origin
                    if (g_ref.valid) {
                        V3 f = g_ref.fwdH; float dist = g_cfg.menuDistance;
                        qp.position = { g_ref.pos.x + f.x * dist, g_ref.pos.y, g_ref.pos.z + f.z * dist };
                        float a = atan2f(-f.x, -f.z);
                        qp.orientation = { 0.0f, sinf(a * 0.5f), 0.0f, cosf(a * 0.5f) };
                    }
                    xr::SubmitScreen(bb, pp, g_cfg.menuWidth);
                    PathNote('S');
                } else PathNote('x');
                bb->Release();
            } else PathNote('-');
        }
    }
    g_eyesThisFrame = false; g_vpThisPresent = false;
    TreeParkAge();
    if (g_cfg.enable == 2 && g_haveDevice) {
        static double last = 0; static int n = 0; double now = NowMs();
        if (last > 0 && ++n >= 300) {
            LOG("FLAT avg %d frames: frame=%.2f ms renderFrame=%.2f ms | bufCreates=%ld (%.1f MB) treeMakes=%ld (%.2f ms/frame) cacheHits=%ld parked=%ld freed=%ld resets=%ld", n, (now - last) / n, g_rfMs / n, g_bufCreates, g_bufBytes / 1048576.0, g_treeMakes, g_treeMakeMs / n, g_treeCacheHits, g_treeParked, g_treeFreed);
            g_treeMakeMs = 0; g_rfMs = 0;
            g_treeParked = g_treeFreed = 0;
            InterlockedExchange(&g_bufCreates, 0); g_bufBytes = 0; g_treeMakes = 0; g_treeCacheHits = 0; n = 0; last = now;
        } else if (last == 0) last = now;
    }
    double tp = NowMs();
    if (g_tRFEnd > 0) g_sec.mid += tp - g_tRFEnd;
    bool capEnding = false;
    if (g_capState == 2) {
        TraceMark("present begin"); capEnding = true;
        ID3D10Texture2D* bb = AcquireBackbuffer();
        if (bb) { SaveTexBmp(bb, "cap_backbuffer.bmp", 2); bb->Release(); }
        g_capCollect = false;
        for (int i = 0; i < g_nCapRTs; ++i) {   // every render target drawn into during the captured frame
            D3D10_TEXTURE2D_DESC td; g_capRTs[i]->GetDesc(&td);
            char nm[64]; snprintf(nm, sizeof nm, "cap_rt%02d_%ux%u_f%d.bmp", i, td.Width, td.Height, (int)td.Format);
            LOG("capture: rt%02d = tex %p %ux%u fmt=%d", i, (void*)g_capRTs[i], td.Width, td.Height, (int)td.Format);
            if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                SaveTexBmp(g_capRTs[i], nm, (int)((td.Width + 767) / 768));
            g_capRTs[i]->Release();
        }
        g_nCapRTs = 0;
        LOG("capture: done (eyes this frame=%d)", g_eyesThisFrame);
        g_capState = 0;
    }
    if (g_capState == 1) { g_capState = 2; g_traceDraws = true; g_drawSeq = 0; g_curRT = 0; g_nSeenSrv = 0; g_nCapRTs = 0; g_capCollect = true; LogWrite("capture: whole-frame draw trace begins after this present"); }
    perf::GpuMark(perf::G_PRESENT);
    perf::SetPhase(perf::P_PRESENT);
    Trace("PRES>");
    o_Present(self, edx, a1);
    if (capEnding) { TraceMark("present end"); g_traceDraws = false; }
    Trace("PRES<");
    if (g_traceLeft > 0) --g_traceLeft;
    { static int armedFrames = 0; if (g_profArmed && ++armedFrames == 900) g_traceLeft = 4; }
    perf::SetPhase(perf::P_SIM);
    perf::GpuMark(perf::G_FRAME);
    g_tPresentEnd = NowMs();
    g_prof.present += g_tPresentEnd - tp;
    g_sec.present += g_tPresentEnd - tp;
    if (g_profArmed && ++g_sec.n >= 90) { SecReport(); if (g_cfg.profile) perf::Report(); }
}


// ---- driver Reset/resize (0x104204A0, __thiscall 7 args, ret 0x1c). A window "resized" event fires it; it then
// unconditionally calls ResizeTarget + ResizeBuffers + rebuilds the backbuffer RTV (GPU flush). When the window
// cannot take the requested size (clamped by the desktop), ResizeTarget re-fires the event every frame -> stall loop.
// Skip calls that would not change anything.
typedef void (__fastcall *DrvReset_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static DrvReset_t o_DrvReset = nullptr;
static void __fastcall Hook_DrvReset(void* self, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                     uint32_t a5, uint32_t a6, uint32_t a7) {
    uint8_t* d = (uint8_t*)self;
    uint32_t sc = *(uint32_t*)(d + 0x40), cw = *(uint32_t*)(d + 0x4c), ch = *(uint32_t*)(d + 0x50);
    uint32_t f68 = *(uint32_t*)(d + 0x68), f6c = *(uint32_t*)(d + 0x6c);
    uint32_t a6e = a6 ? a6 : 1;
    bool same = sc && a1 == cw && a2 == ch && a7 == f6c && a6e == f68;
    static int logged = 0;
    LONG n = InterlockedIncrement(same ? &g_resetSkips : &g_resetCalls);
    if (logged < 20 || (n % 300) == 0) {
        ++logged;
        LogWrite("DrvReset(%u x %u, a3=%u a4=%u a6=%u a7=%u) cur=%ux%u f68=%u f6c=%u -> %s (skips=%ld calls=%ld)",
                 a1, a2, a3, a4, a6, a7, cw, ch, f68, f6c, same ? "SKIP" : "pass", g_resetSkips, g_resetCalls);
    }
    if (same && g_cfg.skipRedundantReset) return;
    o_DrvReset(self, edx, a1, a2, a3, a4, a5, a6, a7);
}

// ---- tree (vegetation) LOD parameter check 0x10399C90 (__thiscall, no args). When either of two
// config floats (cfg+0x6A4, cfg+0x7D8; cfg = *(this+0x18)) differs from the cached copy (this+0x420/+0x424)
// it rebuilds the render data of EVERY tree type (~400 D3D10 buffers). Diagnose and optionally suppress.
typedef void (__fastcall *TreeLod_t)(void*);
static TreeLod_t o_TreeLod = nullptr;
static void __fastcall Hook_TreeLod(void* self) {
    uint8_t* t = (uint8_t*)self;
    uint8_t* cfg = *(uint8_t**)(t + 0x18);
    float c0 = *(float*)(t + 0x420), c1 = *(float*)(t + 0x424);
    float n0 = cfg ? *(float*)(cfg + 0x6A4) : 0, n1 = cfg ? *(float*)(cfg + 0x7D8) : 0;
    InterlockedIncrement(&g_treeCalls);
    bool diff = (c0 != n0) || (c1 != n1);
    if (diff) {
        static int logged = 0;
        if (logged < 40) { ++logged; LogWrite("TreeLod this=%p cfg=%p cached=(%.6g, %.6g) new=(%.6g, %.6g) phase=%s suppress=%d",
                                              self, cfg, c0, c1, n0, n1, g_inEye ? "eye" : "other", g_treeSuppress); }
        if (g_treeSuppress) {
            // Rebuild only for a real change (>2% relative); otherwise keep the existing tree data.
            auto rel = [](float a, float b) { float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); return m > 0 ? fabsf(a - b) / m : 0.f; };
            if (rel(c0, n0) < 0.02f && rel(c1, n1) < 0.02f) { InterlockedIncrement(&g_treeSkipped); return; }
        }
        InterlockedIncrement(&g_treeRebuilds);
    }
    o_TreeLod(self);
}

// ---- tree type render-data factory 0x103970E0 (__thiscall(this, int* key, int data), ret 8) - count per frame
typedef void (__fastcall *TreeMake_t)(void*, void*, int*, int);
static TreeMake_t o_TreeMake = nullptr;
// The engine rebuilds every tree type's render data on every frame: the slot object is destroyed (dtor
// 0x10396CB0 releases ~16 D3D10 buffers), a fresh slot is constructed, and the factory 0x103970E0 rebuilds the
// same geometry from unchanged inputs. Cache: on destruction, park the slot's GPU objects (keyed by the factory
// inputs); when the factory runs with identical inputs, hand them back instead of rebuilding. Parked objects
// that are not reused within a few frames are destroyed normally.
struct TreeSig { uint32_t key0, key1, model, type, flag, cfgA, cfgB, set[8]; };
struct TreeRec { uintptr_t slot; TreeSig sig; uint32_t obj[4]; uint8_t tail[0x6C]; };     // built slots
struct TreePark { bool used; TreeSig sig; uint32_t obj[4]; uint8_t tail[0x6C]; uint32_t frame; };
static TreeRec g_treeRecs[1024];
static TreePark g_treePark[512];
static uint32_t g_presentCount = 0;
typedef void* (__fastcall *RegFind_t)(void*, void*, int*);
typedef void (__fastcall *SlotRelease_t)(void*);
typedef void (__thiscall *VDtor_t)(void*, int);
static bool TreeSignature(void* self, int* key, TreeSig& s) {
    if (!key) return false;
    void* e = ((RegFind_t)A(0x10449550))((void*)A(0x10F97A40), nullptr, key);
    if (!e) return false;
    uint8_t* ent = *(uint8_t**)((uint8_t*)e + 4);
    if (!ent) return false;
    uint8_t* t = (uint8_t*)self; uint8_t* cfg = *(uint8_t**)(t + 0x18);
    uint8_t* flagObj = *(uint8_t**)A(0x1160C2EC);
    memset(&s, 0, sizeof s);
    s.key0 = (uint32_t)key[0]; s.key1 = (uint32_t)key[1];
    s.model = *(uint32_t*)(ent + 0x108); s.type = *(uint32_t*)(ent + 0x100);
    s.flag = flagObj ? flagObj[0x30F] : 0;
    if (cfg) { s.cfgA = *(uint32_t*)(cfg + 0x6A4); s.cfgB = *(uint32_t*)(cfg + 0x7D8); }
    memcpy(s.set, t + 0x38, sizeof s.set);
    return true;
}
static TreeRec* RecFind(uintptr_t slot, bool create) {
    size_t h = (slot >> 3) % 1024;
    for (int i = 0; i < 1024; ++i) {
        TreeRec& r = g_treeRecs[(h + i) % 1024];
        if (r.slot == slot) return &r;
        if (!r.slot) { if (!create) return nullptr; r.slot = slot; return &r; }
    }
    return nullptr;
}
static void RecErase(TreeRec* r) {   // backward-shift delete keeps the probe chains intact
    size_t i = r - g_treeRecs;
    r->slot = 0;
    for (size_t j = (i + 1) % 1024; g_treeRecs[j].slot; j = (j + 1) % 1024) {
        size_t home = (g_treeRecs[j].slot >> 3) % 1024;
        bool between = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
        if (!between) { g_treeRecs[i] = g_treeRecs[j]; g_treeRecs[j].slot = 0; i = j; }
    }
}
static void DestroyObjs(uint32_t* obj) {
    for (int k = 0; k < 4; ++k) if (obj[k]) { void* o = (void*)(uintptr_t)obj[k]; (*(VDtor_t*)(*(uintptr_t*)o))(o, 1); obj[k] = 0; }
}
static void TreeParkAge() {   // once per frame
    ++g_presentCount;
    for (auto& p : g_treePark) if (p.used && g_presentCount - p.frame > 4) { DestroyObjs(p.obj); p.used = false; InterlockedIncrement(&g_treeFreed); }
}
typedef void* (__fastcall *SlotDtor_t)(void*, void*, int);
static SlotDtor_t o_SlotDtor = nullptr;
static volatile LONG g_slotDtors = 0;
static void* __fastcall Hook_SlotDtor(void* self, void* edx, int flags) {
    uint32_t* d = (uint32_t*)self;
    { LONG n = InterlockedIncrement(&g_slotDtors); if (n <= 10 || n % 5000 == 0) LogWrite("SlotDtor #%ld self=%p flags=%d obj=%08x %08x %08x %08x rec=%d", n, self, flags, d[1], d[3], d[4], d[5], RecFind((uintptr_t)self, false) != nullptr); }
    if (TreeRec* r = RecFind((uintptr_t)self, false)) {
        if (g_treeCache && d[1] == r->obj[0] && d[3] == r->obj[1] && d[4] == r->obj[2] && d[5] == r->obj[3] && d[1]) {
            for (auto& p : g_treePark) if (!p.used) {
                p.used = true; p.sig = r->sig; memcpy(p.obj, r->obj, sizeof p.obj); memcpy(p.tail, (uint8_t*)self + 0x18, 0x6C);
                p.frame = g_presentCount; d[1] = d[3] = d[4] = d[5] = 0;   // the original dtor will not destroy them now
                InterlockedIncrement(&g_treeParked);
                break;
            }
        }
        RecErase(r);
    }
    return o_SlotDtor(self, edx, flags);
}
static void __fastcall Hook_TreeMake(void* self, void* edx, int* key, int data) {
    InterlockedIncrement(&g_treeMakes);
    if (g_inEye) InterlockedIncrement(&g_treeMakesEye);
    uint32_t* d = (uint32_t*)(uintptr_t)data;
    { static int n = 0; if (++n <= 120 && (n <= 10 || n > 100)) LogWrite("TreeMake #%d data=%p pre obj=%08x %08x %08x %08x +8=%08x vt=%08x", n, (void*)d, d ? d[1] : 0, d ? d[3] : 0, d ? d[4] : 0, d ? d[5] : 0, d ? d[2] : 0, d ? d[0] : 0); }
    TreeSig sig; bool haveSig = data && TreeSignature(self, key, sig);
    if (haveSig && g_treeCache) {
        for (auto& p : g_treePark) if (p.used && memcmp(&p.sig, &sig, sizeof sig) == 0) {
            ((SlotRelease_t)A(0x10395E40))((void*)d);       // same first step as the factory (slot is fresh: no-op)
            d[1] = p.obj[0]; d[3] = p.obj[1]; d[4] = p.obj[2]; d[5] = p.obj[3];
            memcpy((uint8_t*)d + 0x18, p.tail, 0x6C);
            p.used = false;
            if (TreeRec* r = RecFind((uintptr_t)data, true)) { r->sig = sig; memcpy(r->obj, p.obj, sizeof r->obj); }
            InterlockedIncrement(&g_treeCacheHits);
            return;
        }
    }
    double t0 = NowMs();
    o_TreeMake(self, edx, key, data);
    g_treeMakeMs += NowMs() - t0;
    if (haveSig) if (TreeRec* r = RecFind((uintptr_t)data, true)) { r->sig = sig; r->obj[0] = d[1]; r->obj[1] = d[3]; r->obj[2] = d[4]; r->obj[3] = d[5]; }
}

// ---- near-pass projection 0x10367270 (cdecl(float* out, cam, near, far)). The first-person arms/weapon
// (NearZ passes) get a projection rebuilt from the camera's scalar FOV/aspect with NearPassZMin/Max, which is
// symmetric and therefore wrong for an off-axis eye: the weapon lands in a different place in each eye and
// shows double. For the posed eye camera, build the eye's exact off-axis projection with the near-pass range.
typedef float* (__cdecl *NearProj_t)(float*, uint8_t*, float, float);
typedef void (__cdecl *ProjPost_t)(float*, float);
static NearProj_t o_NearProj = nullptr;
static float* __cdecl Hook_NearProj(float* out, uint8_t* cam, float n, float f) {
    if (!g_eyeCam || cam != g_eyeCam || !g_cfg.fixNearPass) return o_NearProj(out, cam, n, f);
    float tl = g_eyeTan[0], tr = g_eyeTan[1], td = g_eyeTan[2], tu = g_eyeTan[3];
    memset(out, 0, 64);
    out[0] = 2.0f / (tr - tl);
    out[5] = 2.0f / (tu - td);
    out[8] = (tr + tl) / (tr - tl);
    out[9] = (tu + td) / (tu - td);
    out[10] = f / (n - f);
    out[11] = -1.0f;
    out[14] = n * f / (n - f);
    ((ProjPost_t)A(0x1040D790))(out, *(float*)(cam + 0x224));   // same depth post-step as the original
    InterlockedIncrement(&g_nearProjEye);
    { static int logged = 0; if (logged++ < 2) LOG("near pass projection: near %.3f far %.3f", n, f); }
    return out;
}

// ============================================================ diagnostics (active even when Enable=0)
static void ModOff(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, '\\'); b = b ? b + 1 : name;
        uintptr_t va = (_stricmp(b, "Dunia.dll") == 0) ? (a - (uintptr_t)m + 0x10000000) : (a - (uintptr_t)m);
        snprintf(out, n, "%s+%x%s", b, (unsigned)va, _stricmp(b, "Dunia.dll") == 0 ? "(VA)" : "");
    } else snprintf(out, n, "%08x", (unsigned)a);
}
static void LogDuniaStack(const uintptr_t* sp, int words) {
    int found = 0;
    for (int i = 0; i < words && found < 12; ++i) {
        uintptr_t v = 0;
                MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        v = sp[i];
        if (g_base && v > g_base + 0x1000 && v < g_base + 0xE00000) {
            char s[96]; ModOff(v, s, sizeof s); LogWrite("   stack[%d] %s", i, s); ++found;
        }
    }
}
static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 /*thread name*/ || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;            // C++ throw (handled by the game)
    char where[96]; ModOff((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    if (strstr(where, "SPFILEQ") || strstr(where, "SETUPAPI")) return EXCEPTION_CONTINUE_SEARCH; // device enumeration noise
    static LONG count = 0;
    if (InterlockedIncrement(&count) > 40) return EXCEPTION_CONTINUE_SEARCH;
    LogWrite("EXCEPTION %08lx at %s thread=%lu info=%08lx %08lx", code, where, GetCurrentThreadId(),
             ep->ExceptionRecord->NumberParameters > 0 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[0] : 0ul,
             ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul);
    LogDuniaStack((const uintptr_t*)ep->ContextRecord->Esp, 512);
    return EXCEPTION_CONTINUE_SEARCH;
}


typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE, UINT);
typedef VOID (WINAPI *ExitProcess_t)(UINT);
static TerminateProcess_t o_TerminateProcess = nullptr;
static ExitProcess_t o_ExitProcess = nullptr;
static BOOL WINAPI Hook_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        LogWrite("TerminateProcess(self, code=%u) thread=%lu; Dunia frames:", code, GetCurrentThreadId());
        uintptr_t esp; __asm__ volatile("movl %%esp, %0" : "=r"(esp));
        LogDuniaStack((const uintptr_t*)esp, 4096);
    }
    return o_TerminateProcess(h, code);
}
static VOID WINAPI Hook_ExitProcess(UINT code) {
    LogWrite("ExitProcess(code=%u) thread=%lu; Dunia frames:", code, GetCurrentThreadId());
    uintptr_t esp; __asm__ volatile("movl %%esp, %0" : "=r"(esp));
    LogDuniaStack((const uintptr_t*)esp, 4096);
    o_ExitProcess(code);
}
static void HookExport(const char* mod, const char* fn, void* hook, void** orig) {
    uint8_t* p = (uint8_t*)GetProcAddress(GetModuleHandleA(mod), fn);
    static const uint8_t hot[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };   // mov edi,edi; push ebp; mov ebp,esp
    if (!p) { LogWrite("hook %s!%s: not found", mod, fn); return; }
    if (memcmp(p, hot, 5) != 0) { LogWrite("hook %s!%s: prologue %02x %02x %02x %02x %02x, skipped", mod, fn, p[0], p[1], p[2], p[3], p[4]); return; }
    bool ok = PatchJmp((uintptr_t)p, hot, hook, orig);
    LogWrite("hook %s!%s: %d", mod, fn, ok);
}

// ============================================================ install / proxy
static bool g_installed = false;
static void Install() {
    if (g_installed) return;
    g_installed = true;
    HMODULE dunia = GetModuleHandleA("Dunia.dll");
    if (!dunia) { LOG("Dunia.dll not loaded"); g_installed = false; return; }
    g_base = (uintptr_t)dunia;
    LOG("Dunia.dll base %08x", (unsigned)g_base);
    if (gameaddr::Detect() == gameaddr::GOG) LOG("GOG build: engine addresses translated from the Steam map");
    if (!g_cfg.enable) { LOG("disabled by config"); return; }
    InstallFramePacing();
    bool ok1 = PatchJmp(A(eng::RenderViewport), eng::RenderViewportPrologue, (void*)&Hook_RenderViewport, (void**)&o_RenderViewport);
    bool ok2 = PatchJmp(A(eng::PresentWrapper), eng::PresentPrologue, (void*)&Hook_Present, (void**)&o_Present);
    static const uint8_t rfPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };   // push ebp; mov ebp,esp; and esp,-16
    bool ok3 = PatchJmpN(A(0x10345C70), rfPrologue, 6, (void*)&Hook_RenderFrame, (void**)&o_RenderFrame);
    InstallNpcFix(); InstallReflFix(); InstallShadowShare(); if (g_cfg.scopeAmmoOwn) InstallAmmoHook(); InstallHudHook();
    {
        controls::Config cc;
        cc.enable = (int)IniF("MotionControls", 1); cc.rumble = IniF("ControllerRumble", 1.0f);
        cc.stickPitch = (int)IniF("TurnStickPitch", 0); cc.holdMs = (int)IniF("ButtonHoldMs", 400); cc.turnFull = (int)IniF("TurnFullSpeed", 1); cc.snapTurn = IniF("SnapTurn", 45.0f); cc.throwArc = (int)IniF("ThrowArc", 1); cc.throwArcRight = (int)IniF("ThrowArcRight", 1); cc.macheteTrigger = (int)IniF("MacheteTriggerAttack", 0); cc.monocular = (int)IniF("Monocular", 1); cc.flashlight = (int)IniF("Flashlight", 0); cc.moveDir = (int)IniF("MovementDirection", 0); cc.turretInvertY = (int)IniF("TurretInvertY", 0); cc.recenterHoldMs = (int)IniF("RecenterHoldMs", 1000); controls::SetTurnSpeed(IniF("SmoothTurnSpeed", 1.0f)); cc.flashRadius = IniF("FlashlightGestureRadius", 0.13f);
        cc.shoulderFwd = IniF("ShoulderZoneFwd", -0.02f); cc.shoulderUp = IniF("ShoulderZoneUp", -0.20f);
        cc.hipUp = IniF("HipZoneUp", -0.45f); cc.chestUpMin = IniF("ChestZoneUpMin", -0.55f); cc.chestUpMax = IniF("ChestZoneUpMax", -0.10f);
        cc.logZones = (int)IniF("LogHolsterZones", 0);
        cc.sightNeedsGrip = (int)IniF("ScopeNeedsGrip", 1);
        cc.sightOn = IniF("SightOnDistance", 0.25f); cc.sightOff = IniF("SightOffDistance", 0.35f);
        controls::Install(dunia, cc);
        arms::Config ac;
        ac.weaponInHand = (int)IniF("WeaponInHand", 1); ac.aimFromController = (int)IniF("AimFromController", 1);
        ac.dump = (int)IniF("DumpSkeleton", 0);
        ac.ik = (int)IniF("ArmIK", 1); ac.freeLeftHand = (int)IniF("FreeLeftHand", 1);
        ac.shoulderSide = IniF("ShoulderSide", 0.17f); ac.shoulderDown = IniF("ShoulderDown", 0.22f); ac.shoulderBack = IniF("ShoulderBack", 0.06f);
        ac.twoHandMaxSpan = IniF("TwoHandMaxSpan", 0.65f); ac.noIdleBreak = (int)IniF("NoIdleAnimation", 1);
        xr::SetHoldLostPose(IniF("HoldLostTracking", 1) != 0);
        g_cfg.grabDistance = IniF("GrabDistance", 0.15f);
        g_cfg.scopeInstant = (int)IniF("ScopeInstant", 1); g_cfg.scopeNeedsGrip = (int)IniF("ScopeNeedsGrip", 1);
        g_cfg.scopeGameReticle = (int)IniF("ScopeGameReticle", 1); g_cfg.scopeAmmoSize = IniF("ScopeAmmoSize", 0.28f);
        g_cfg.scopeAmmoX = IniF("ScopeAmmoX", 0.59f); g_cfg.scopeAmmoMask = (int)IniF("ScopeAmmoMask", 1); g_cfg.scopeAmmoKey = (int)IniF("ScopeAmmoKey", 12); if (g_cfg.scopeAmmoKey < 1) g_cfg.scopeAmmoKey = 1; g_cfg.scopeAmmoLight = (int)IniF("ScopeAmmoLight", 90); g_cfg.scopeAmmoOwn = (int)IniF("ScopeAmmoOwn", 1); g_cfg.hidePrompts = (int)IniF("HideInteractionPrompts", 1); g_cfg.hideHurt = (int)IniF("HideHurtOverlay", 1); g_cfg.ironSights = (int)IniF("IronSights", 0); g_cfg.scopeTestMs = IniF("ScopeTestMs", 500.0f); g_cfg.scopeAmmoY = IniF("ScopeAmmoY", -0.30f);
        g_cfg.scopeRound = (int)IniF("ScopeRound", 1); g_cfg.scopeRoundSize = IniF("ScopeRoundSize", 0.8f);
        g_cfg.scopeHudX = IniF("ScopeHudX", 0.83f); g_cfg.scopeHudY = IniF("ScopeHudY", 0.925f);
        xr::SetScopeCrosshair((int)IniF("ScopeCrosshair", 2));
        g_cfg.scopeFromWeapon = (int)IniF("ScopeFromWeapon", 1); g_cfg.scopeSmoothing = IniF("ScopeSmoothing", 0.06f);
        g_cfg.testCheats = (int)IniF("TestCheats", 0); { char ip[MAX_PATH]; snprintf(ip, MAX_PATH, "%sfc2vr.ini", g_dir); arms::SetIniPath(ip); } g_cfg.reticleDot = (int)IniF("ReticleDot", 1); g_cfg.reticleDistance = IniF("ReticleDistance", 10.0f); g_cfg.reticleSize = IniF("ReticleSize", 0.06f);
        ac.handOfs[0] = IniF("HandOffsetX", 0); ac.handOfs[1] = IniF("HandOffsetY", 0); ac.handOfs[2] = IniF("HandOffsetZ", 0);
        ac.leftHandRoll = IniF("LeftHandRoll", 45.0f); ac.macheteAngle = IniF("MacheteAngle", 60.0f);
        ac.leftHandYaw = IniF("LeftHandYaw", 30.0f); ac.leftForearmTwist = IniF("LeftForearmTwist", 1.0f); ac.armsIndependent = (int)IniF("ArmsIndependent", 1); ac.steadyHands = (int)IniF("SteadyHands", 1); ac.grabBoxShrink = IniF("GrabBoxShrink", 0.25f); ac.upperArmTwist = IniF("UpperArmTwist", 0.5f); ac.macheteYaw = IniF("MacheteYaw", 15.0f); ac.macheteRoll = IniF("MacheteRoll", 0.0f); ac.mapAngle = IniF("MapPitch", 0.0f); ac.mapYaw = IniF("MapYaw", 0.0f); ac.mapRoll = IniF("MapRoll", 0.0f); ac.freePitch = IniF("FreeHandPitch", 0.0f); ac.freeYaw = IniF("FreeHandYaw", 0.0f); ac.freeRoll = IniF("FreeHandRoll", 0.0f); ac.phonePitch = IniF("PhonePitch", ac.freePitch); ac.phoneYaw = IniF("PhoneYaw", ac.freeYaw); ac.phoneRoll = IniF("PhoneRoll", ac.freeRoll); ac.phoneDiag = (int)IniF("PhoneDiag", 0);
        ac.handGrab = (int)IniF("HandGrab", 1); ac.grabRadius = IniF("GrabRadius", 0.35f); ac.grabReach = IniF("GrabReach", 0.06f); ac.pickupReach = IniF("PickupReach", 0.20f); ac.grabPhysics = (int)IniF("GrabPhysics", 1); ac.grabMaxMass = IniF("GrabMaxMass", 25.0f); ac.grabMaxSize = IniF("GrabMaxSize", 0.8f); ac.throwScale = IniF("ThrowScale", 1.3f); ac.bodyRadius = IniF("BodyRadius", 0.30f); ac.climbing = (int)IniF("Climbing", 0); ac.climbReach = IniF("ClimbReach", 0.10f); ac.climbMode = (int)IniF("ClimbMode", 1); ac.mantleBelowHead = IniF("MantleBelowHead", 0.55f); ac.climbGrace = IniF("ClimbGrace", 1.5f); ac.climbTurnSpeed = IniF("ClimbTurnSpeed", 120.0f); ac.watchStickSign = IniF("WatchStickSign", 1.0f); ac.throwSpeed = IniF("ThrowSpeed", 0.0f); ac.throwRange = IniF("ThrowRange", 1.5f); ac.npcGrab = (int)IniF("NpcGrab", 0); ac.bodyClock = (int)IniF("HandsOnBodyClock", 1); ac.armSmooth = IniF("ArmSmoothing", 0.0f); ac.clavSmooth = IniF("ClavicleSmoothing", 0.5f); ac.localFrame = (int)IniF("ArmLocalFrame", 1); g_framePacing = (int)IniF("FramePacing", 1); ac.foregripSmooth = IniF("ForegripSmoothing", 0.3f); ac.twistFadeFrom = IniF("UpperArmTwistFadeFrom", -1.0f); ac.twistFadeTo = IniF("UpperArmTwistFadeTo", 0.0f); ac.twistSmooth = IniF("UpperArmTwistSmoothing", 0.0f); ac.softIk = IniF("SoftIkStart", 0.0f); ac.softIkMaxPull = IniF("SoftIkMaxShoulderPull", 0.12f); ac.elbowSmooth = IniF("ElbowSmoothing", 0.0f); g_camBodyClock = (int)IniF("CameraOnBodyClock", 1); g_motionDiag = (int)IniF("MotionDiag", 0); ac.roomScale = (int)IniF("RoomScale", 0); ac.roomLeash = IniF("RoomScaleLeash", 0.10f); ac.roomMaxSpeed = IniF("RoomScaleMaxSpeed", 4.0f); ac.roomRadius = IniF("RoomScaleWallDistance", 0.30f); g_rsDelay = (int)IniF("RoomScaleViewDelay", 1); g_rsOn = ac.roomScale != 0; ac.contactPush = (int)IniF("ContactPush", 1); ac.freezeReload = (int)IniF("FreezeReload", 1); ac.macheteHits = (int)IniF("MacheteHits", 1); ac.macheteSpeed = IniF("MacheteSwingSpeed", 2.5f); ac.macheteCooldown = IniF("MacheteCooldown", 0.5f); ac.macheteSound = (int)IniF("MacheteSwingSound", 1); ac.seatWeapon = (int)IniF("SeatWeapon", 1); ac.macheteSoundSpeed = IniF("MacheteSoundSpeed", 6.0f); ac.flashRange = IniF("FlashlightRange", 30.0f); ac.flashInner = IniF("FlashlightInnerAngle", 18.0f); ac.flashOuter = IniF("FlashlightOuterAngle", 32.0f); ac.flashBright = IniF("FlashlightBrightness", 1.0f); ac.noColorFilter = (int)IniF("NoColorFilter", 0); ac.noRimLight = (int)IniF("NoRimLight", 0);
        { char b[64], ip[MAX_PATH]; snprintf(ip, MAX_PATH, "%sfc2vr.ini", g_dir); GetPrivateProfileStringA("VR", "MacheteSoundIds", "", b, sizeof b, ip); unsigned a1 = 0, a2 = 0; int n = b[0] ? sscanf(b, "%x,%x", &a1, &a2) : 0;
          if (n >= 1) { ac.macheteSnd[0] = a1; ac.macheteSnd[1] = n >= 2 ? a2 : 0; } } ac.bladeLen = IniF("MacheteBladeLength", 0.55f); ac.pushMinSpeed = IniF("PushMinSpeed", 0.3f); ac.pushRadius = IniF("PushRadius", 0.06f); ac.pushScale = IniF("PushScale", 1.0f); ac.pushMass = IniF("PushMass", 40.0f); ac.pushWeaponLen = IniF("PushWeaponLen", 0.7f); ac.npcReach = IniF("NpcReach", 0.25f); ac.npcGain = IniF("NpcPullGain", 0.6f); ac.npcMaxSpeed = IniF("NpcMaxSpeed", 10.0f); ac.npcRagdollTime = IniF("NpcRagdollTime", 2.5f); ac.npcActive = (int)IniF("NpcActive", 1); ac.npcActiveWeight = IniF("NpcActiveWeight", 1.0f); ac.npcStiffness = IniF("NpcStiffness", 1.0f); ac.npcKnockPull = IniF("NpcKnockPull", 1.12f); ac.npcKnockDown = IniF("NpcKnockDown", 0.35f); ac.npcLeadGain = IniF("NpcLeadGain", 8.0f); ac.npcLeadSpeed = IniF("NpcLeadSpeed", 4.5f); ac.npcKnockSpeed = IniF("NpcKnockSpeed", 2.1f); ac.weaponRollFix = (int)IniF("WeaponRollFix", 0); ac.climbHolster = (int)IniF("ClimbHolster", 1); ac.gripInteract = (int)IniF("GripInteract", 0); ac.throwMaxSpeed = IniF("ThrowMaxSpeed", 25.0f); ac.useReach = IniF("UseReach", 0.9f); ac.mouthReach = IniF("MouthReach", 0.18f);
        ac.backpackFwd = IniF("BackpackZoneFwd", 0.0f); ac.backpackUp = IniF("BackpackZoneUp", -0.25f);
        if (cc.enable) { arms::Install(g_base, ac, &PatchJmpN); arms::SetUseCallback(&controls::PulseUse); }
    }
    InstallDevConsole();
    static const uint8_t rsPrologue[8] = { 0x51, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x40, 0x00 };  // push ecx; push esi; mov esi,ecx; cmp [esi+40],0
    bool ok4 = PatchJmpN(A(0x104204A0), rsPrologue, 8, (void*)&Hook_DrvReset, (void**)&o_DrvReset);
    static const uint8_t tlPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };   // push ebp; mov ebp,esp; and esp,-8
    bool ok5 = PatchJmpN(A(0x10399C90), tlPrologue, 6, (void*)&Hook_TreeLod, (void**)&o_TreeLod);
    static const uint8_t tmPrologue[7] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x30 };   // mov eax,[esp+4]; sub esp,30h
    bool ok6 = PatchJmpN(A(0x103970E0), tmPrologue, 7, (void*)&Hook_TreeMake, (void**)&o_TreeMake);
    static uint8_t sdPrologue[9] = { 0x56, 0x8B, 0xF1, 0xC7, 0x06, 0, 0, 0, 0 };   // push esi; mov esi,ecx; mov [esi],vtable 10E48C8C
    *(uint32_t*)(sdPrologue + 5) = (uint32_t)A(0x10E48C8C);
    bool ok7 = PatchJmpN(A(0x10396CB0), sdPrologue, 9, (void*)&Hook_SlotDtor, (void**)&o_SlotDtor);
    LOG("hook TreeLod=%d TreeMake=%d SlotDtor=%d", ok5, ok6, ok7);
    static const uint8_t npPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };   // push ebp; mov ebp,esp; and esp,-16
    bool ok8 = PatchJmpN(A(0x10367270), npPrologue, 6, (void*)&Hook_NearProj, (void**)&o_NearProj);
    LOG("hook NearProj=%d", ok8);
    static const uint8_t r2Prologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    bool ok9 = PatchJmpN(A(0x10341BB0), r2Prologue, 6, (void*)&Hook_Render2DView, (void**)&o_Render2DView);
    LOG("hook Render2DView=%d", ok9);
    LOG("hooks: RenderViewport=%d Present=%d RenderFrame=%d DrvReset=%d", ok1, ok2, ok3, ok4);
}

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t o_DI8Create = nullptr;

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    if (!o_DI8Create) {
        char path[MAX_PATH]; GetSystemDirectoryA(path, MAX_PATH); strcat(path, "\\dinput8.dll");
        HMODULE real = LoadLibraryA(path);
        o_DI8Create = real ? (DirectInput8Create_t)GetProcAddress(real, "DirectInput8Create") : nullptr;
        LOG("real dinput8: %s %p", path, (void*)o_DI8Create);
        Install();
    }
    return o_DI8Create ? o_DI8Create(h, v, r, o, u) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char modulePath[MAX_PATH]; memcpy(modulePath, g_dir, sizeof(modulePath)); modulePath[MAX_PATH - 1] = 0;
        char* s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
        InitializeCriticalSection(&g_logCs);
        char lp[MAX_PATH]; snprintf(lp, MAX_PATH, "%sfc2vr.log", g_dir);
        { char pp[MAX_PATH]; snprintf(pp, MAX_PATH, "%sfc2vr_prev.log", g_dir); MoveFileExA(lp, pp, MOVEFILE_REPLACE_EXISTING); }   // keep the last session's log
        g_log = fopen(lp, "w");
        LOG("FC2VR loaded (v232 test 10 = 1.0.2 + clavicle/foregrip smoothing (F4) + frame pacing (Shift+F4) + no steady camera while riding, Insert/PgDn DLSS A/B, local arm frame (Ctrl+F5 A/B), Ctrl+F4 GPU capture, Ctrl+Shift+F4 motion capture, build " __DATE__ " " __TIME__ ", module=%s)", modulePath);
        LogVA("startup");
        {   // v225: the game doesn't start on more than 16 logical processors. This DLL loads before any game code (Dunia
            // imports dinput8), so the limit is applied here - FarCry2VR.exe / FarCry2.exe started any way get it.
            char ip[MAX_PATH]; snprintf(ip, MAX_PATH, "%sfc2vr.ini", g_dir);
            int lim = (int)GetPrivateProfileIntA("VR", "CoreLimit", 16, ip);
            DWORD_PTR pm = 0, sm = 0;
            if (lim > 0 && GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) {
                DWORD_PTR m = 0; int n = 0, all = 0;
                for (int b = 0; b < (int)(sizeof(DWORD_PTR) * 8); ++b) if (pm & ((DWORD_PTR)1 << b)) { ++all; if (n < lim) { m |= (DWORD_PTR)1 << b; ++n; } }
                if (m && m != pm) LOG("core limit: %d of %d logical processors (%s)", n, all, SetProcessAffinityMask(GetCurrentProcess(), m) ? "set" : "failed");
                else LOG("core limit: %d logical processors in use (limit %d)", all, lim);
            }
        }
        { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcToMs = 1000.0 / (double)f.QuadPart; }
        { char ip[MAX_PATH]; snprintf(ip, MAX_PATH, "%sfc2vr.ini", g_dir); diag::Startup(ip); }   // v230
        LoadConfig(); g_cullMode = g_cfg.cullMode; g_fovMode = g_cfg.symmetricFov ? ((g_cfg.overscanH > 0 || g_cfg.overscanV > 0) ? 2 : 1) : 0;
        LOG("tracking frame height-direction-1: HeightOffset=%.4f (positions only; directions untranslated)", g_cfg.heightOffset);
        // Windows display scaling makes this DPI-unaware game see a small virtual desktop. Its 1920x1080 window
        // then cannot fit, the size check fails every frame and fires the window-resized event each frame, which
        // (among other things) rebuilds all tree render data (~6 ms/frame). Declaring DPI awareness fixes the cause.
        if (g_cfg.dpiAware) {
            typedef BOOL (WINAPI *SetCtx_t)(HANDLE);
            HMODULE u32 = LoadLibraryA("user32.dll");
            SetCtx_t setCtx = u32 ? (SetCtx_t)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
            BOOL ok = setCtx ? setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */) : FALSE;
            if (!ok) ok = SetProcessDPIAware();
            LOG("DPI awareness: %s", ok ? "on" : "failed");
        }
        AddVectoredExceptionHandler(1, Veh);
        HookExport("kernelbase.dll", "TerminateProcess", (void*)&Hook_TerminateProcess, (void**)&o_TerminateProcess);
        HookExport("kernel32.dll", "ExitProcess", (void*)&Hook_ExitProcess, (void**)&o_ExitProcess);
        hang::Start(g_dir, g_cfg.hangWatch != 0);
        Install();   // Dunia.dll is mapped and relocated when its import (us) initialises
    } else if (reason == DLL_PROCESS_DETACH) {
        LOG("process exiting (thread %lu); Dunia frames on this stack:", GetCurrentThreadId());
        uintptr_t esp; __asm__ volatile("movl %%esp, %0" : "=r"(esp));
        LogDuniaStack((const uintptr_t*)esp, 4096);
    }
    return TRUE;
}

static SHORT DevKey(int vk) { return g_cfg.devKeys ? GetAsyncKeyState(vk) : 0; }
