// xr.cpp - OpenXR session, D3D11 device, keyed-mutex shared eye textures, layer submission.
#include "xr.h"
#include "log.h"
#include "diag.h"
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <d3dcompiler.h>
#include "common/xr_runtime_probe.h"

namespace xr {

// ---------------------------------------------------------------- loader / function table
static HMODULE g_loader = nullptr;
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr = nullptr;

#define XR_FUNCS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) X(xrCreateSession) X(xrDestroySession) \
    X(xrBeginSession) X(xrEndSession) X(xrPollEvent) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrLocateViews) X(xrCreateReferenceSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) X(xrEnumerateViewConfigurationViews) X(xrGetInstanceProperties) \
    X(xrGetSystemProperties) X(xrResultToString) X(xrRequestExitSession) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrCreateActionSpace) X(xrSyncActions) X(xrGetActionStateFloat) \
    X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrLocateSpace) \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback) X(xrGetCurrentInteractionProfile) X(xrPathToString)

#define XR_DECL(name) static PFN_##name p_##name = nullptr;
XR_FUNCS(XR_DECL)
static PFN_xrGetD3D11GraphicsRequirementsKHR p_xrGetD3D11GraphicsRequirementsKHR = nullptr;

// ---------------------------------------------------------------- state
static XrInstance  g_inst = XR_NULL_HANDLE;
static XrSystemId  g_sys = XR_NULL_SYSTEM_ID;
static XrSession   g_sess = XR_NULL_HANDLE;
static XrSpace     g_local = XR_NULL_HANDLE;
static XrSpace     g_view = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool        g_running = false;
static char        g_err[256] = "";
static bool        g_fatal = false;   // a failure that retrying cannot fix: do not churn sessions

static ID3D10Device*        g_dev10 = nullptr;
static ID3D11Device*        g_dev11 = nullptr;
static ID3D11DeviceContext* g_ctx11 = nullptr;

static UINT g_w = 0, g_h = 0;                    // backbuffer (screen quad)
static DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;
static bool g_bbMsaa = false;
static UINT g_eyeW = 0, g_eyeH = 0;              // eye render targets
static UINT g_outW = 0, g_outH = 0;              // DLSS upscaling: the eye swapchains' (output) size, 0 = the eye target size
static DXGI_FORMAT g_eyeFormat = DXGI_FORMAT_UNKNOWN;
static uint32_t g_recW = 0, g_recH = 0;          // runtime-recommended eye size
static DXGI_FORMAT g_eyeChainFmt = DXGI_FORMAT_UNKNOWN;

struct Shared {
    ID3D10Texture2D*  tex10 = nullptr;   // owned by the game's D3D10 device
    ID3D11Texture2D*  tex11 = nullptr;   // same memory opened on our D3D11 device
};
static Shared g_shared[5];   // 0,1 = eyes, 2 = screen quad, 3 = HUD, 4 = reticle

struct Chain {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    int32_t w = 0, h = 0;
};
static Chain g_eye[2];
static Chain g_screen;
static Chain g_hud, g_ret;
static bool g_retOn = false; static XrPosef g_retPose{}; static float g_retWidth = 0.1f, g_retFrac = 0.3f;
// aiming dot: a tiny static texture shown as a quad
static Chain g_dot; static ID3D11Texture2D* g_dotTex = nullptr;
static bool g_dotOn = false; static XrPosef g_dotPose{}; static float g_dotSize = 0.06f;
static Chain g_menu; static ID3D11Texture2D* g_menuTex = nullptr; static bool g_menuOn = false, g_menuDirty = false; static XrPosef g_menuPose{}; static float g_menuWidth = 0.6f;
static uint32_t g_menuPx[MenuW * MenuH];
static Chain g_ammo; static ID3D11Texture2D* g_ammoTex = nullptr; static bool g_ammoDirty = false, g_ammoHave = false; static uint32_t g_ammoPx[AmmoW * AmmoH];
// scope: round window + crosshair drawn over the (square-cropped) scope view
static Chain g_vig; static ID3D11Texture2D* g_vigTex = nullptr; static ID3D11Texture2D* g_vigPlainTex = nullptr; static int g_scopeCross = 2;
void SetScopeCrosshair(int style) { g_scopeCross = style; }
static float g_monoScale = 1.15f;
static ID3D11Texture2D* g_vigMonoTex[2] = {}; static bool g_monoTarget = false; static float g_monoRing = 0.90f;
void SetMonoRingSize(float k) { g_monoRing = k < 0.05f ? 0.05f : k > 0.97f ? 0.97f : k; }
void SetMonoTarget(bool on) { g_monoTarget = on; }
void SetMonoViewScale(float k) { g_monoScale = k < 0.5f ? 0.5f : k > 3.0f ? 3.0f : k; }
static bool g_hudOn = false; static XrPosef g_hudPose{}; static float g_hudWidth = 1.0f; static bool g_hudView = false;
// v227: the HUD's top strip (objective updates, tutorial and system messages) is shown as its own quad, lowered toward
// the upper middle of the view (at the very top of the HUD they sat at the edge of the headset's view)
static float g_hudBand = 0.22f, g_hudDrop = 0.16f; static bool g_hudBandOn = false;
void SetHudTopBandOn(bool on) { g_hudBandOn = on; }
void SetHudTopBand(float band, float drop) { g_hudBand = band < 0 ? 0 : band > 0.5f ? 0.5f : band; g_hudDrop = drop < 0 ? 0 : drop > 0.5f ? 0.5f : drop; }
static bool  g_ready = false;
static bool  g_frameBegun = false;

const char* LastError() { return g_err; }
bool Ready() { return g_ready; }
bool Fatal() { return g_fatal; }
bool SessionRunning() { return g_ready && g_running; }

static bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char s[XR_MAX_RESULT_STRING_SIZE] = "";
    if (p_xrResultToString && g_inst) p_xrResultToString(g_inst, r, s);
    snprintf(g_err, sizeof(g_err), "%s failed: %d %s", what, (int)r, s);
    LOG("XR: %s", g_err);
    return false;
}

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- helpers
static DXGI_FORMAT TypelessFamily(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return f;
    }
}
static DXGI_FORMAT SrgbOf(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM) return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM) return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return f;
}

static bool LoadLoader() {
    if (g_loader) return true;
    char path[MAX_PATH]; HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadLoader, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) slash[1] = 0;
    strcat(path, "openxr_loader.dll");
    // A registered ReShade x86 OpenXR API layer fails to load on this machine and makes
    // xrCreateInstance return XR_ERROR_FILE_ACCESS_ERROR (-32). Disable it for this process only.
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_reshade_1", "1");
    g_loader = LoadLibraryA(path);
    if (!g_loader) { snprintf(g_err, sizeof(g_err), "cannot load %s (%lu)", path, GetLastError()); LOG("XR: %s", g_err); return false; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) { snprintf(g_err, sizeof(g_err), "xrGetInstanceProcAddr missing"); return false; }
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&p_xrCreateInstance);
    return p_xrCreateInstance != nullptr;
}

static bool LoadInstanceFuncs() {
#define XR_LOAD(name) if (XR_FAILED(p_xrGetInstanceProcAddr(g_inst, #name, (PFN_xrVoidFunction*)&p_##name))) { LOG("XR: missing %s", #name); return false; }
    XR_FUNCS(XR_LOAD)
#undef XR_LOAD
    return XR_SUCCEEDED(p_xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR",
                        (PFN_xrVoidFunction*)&p_xrGetD3D11GraphicsRequirementsKHR));
}

// v230: timing wrappers (PACE diagnostics) and XR_FB_display_refresh_rate
static PFN_xrWaitFrame o_xrWaitFrame; static PFN_xrEndFrame o_xrEndFrame;
static PFN_xrEnumerateDisplayRefreshRatesFB p_xrEnumerateDisplayRefreshRatesFB;
static PFN_xrGetDisplayRefreshRateFB p_xrGetDisplayRefreshRateFB;
static PFN_xrRequestDisplayRefreshRateFB p_xrRequestDisplayRefreshRateFB;
static bool g_hasRefreshExt;
static float g_wantRefresh;   // ini DisplayRefreshRate (0 = leave it to the runtime)
volatile double g_displayPeriod = 0;   // v232: seconds, from xrWaitFrame (frame pacing)
double DisplayPeriod() { return g_displayPeriod; }
static double QpcMs() { static double k = 0; if (!k) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); k = 1000.0 / (double)f.QuadPart; } LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * k; }
static XrResult XRAPI_CALL W_xrWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    double t0 = QpcMs(); XrResult r = o_xrWaitFrame(s, wi, fs); double t1 = QpcMs();
    if (XR_SUCCEEDED(r) && fs) diag::OnWaitFrame(t1 - t0, (int64_t)fs->predictedDisplayPeriod, fs->shouldRender != XR_FALSE);
    if (XR_SUCCEEDED(r) && fs && fs->predictedDisplayPeriod > 0) g_displayPeriod = (double)fs->predictedDisplayPeriod * 1e-9;
    return r;
}
static XrResult XRAPI_CALL W_xrEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    diag::GpuFrameEnd();
    double t0 = QpcMs(); XrResult r = o_xrEndFrame(s, ei); diag::OnEndFrame(QpcMs() - t0);
    return r;
}
static void LogRefreshRates(const char* when) {
    if (!g_hasRefreshExt || !p_xrGetDisplayRefreshRateFB || !g_sess) return;
    float cur = 0; p_xrGetDisplayRefreshRateFB(g_sess, &cur);
    char list[160] = ""; size_t len = 0;
    uint32_t n = 0;
    if (p_xrEnumerateDisplayRefreshRatesFB && XR_SUCCEEDED(p_xrEnumerateDisplayRefreshRatesFB(g_sess, 0, &n, nullptr)) && n && n < 32) {
        float r[32]; p_xrEnumerateDisplayRefreshRatesFB(g_sess, n, &n, r);
        for (uint32_t i = 0; i < n && len < sizeof(list) - 12; i++) len += snprintf(list + len, sizeof(list) - len, "%s%.0f", i ? " " : "", r[i]);
    }
    LOG("XR: display refresh %.1f Hz (%s), headset supports: %s", cur, when, len ? list : "?");
}

static bool CreateD3D11(const LUID& luid) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    IDXGIAdapter1* chosen = nullptr; IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (!chosen && memcmp(&d.AdapterLuid, &luid, sizeof(LUID)) == 0) { chosen = a; continue; }
        a->Release();
    }
    fac->Release();
    if (!chosen) { snprintf(g_err, sizeof(g_err), "no adapter matches XR LUID"); LOG("XR: %s", g_err); return false; }
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 3, D3D11_SDK_VERSION,
                                   &g_dev11, nullptr, &g_ctx11);
    if (FAILED(hr)) { // 11_1 not accepted on some systems
        hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl + 1, 2, D3D11_SDK_VERSION,
                               &g_dev11, nullptr, &g_ctx11);
    }
    DXGI_ADAPTER_DESC1 d; chosen->GetDesc1(&d);
    LOG("XR: D3D11 device on adapter '%ls' hr=%08lx", d.Description, (unsigned long)hr);
    chosen->Release();
    return SUCCEEDED(hr);
}

static ID3D10Query* g_evt10 = nullptr;

static bool CreateShared(Shared& s, UINT w, UINT h, DXGI_FORMAT fmt) {
    D3D10_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
    HRESULT hr = g_dev10->CreateTexture2D(&td, nullptr, &s.tex10);
    if (FAILED(hr)) { LOG("XR: D3D10 shared CreateTexture2D hr=%08lx fmt=%d %ux%u", (unsigned long)hr, (int)fmt, w, h); return false; }
    IDXGIResource* res = nullptr; HANDLE sh = nullptr;
    s.tex10->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    hr = res ? res->GetSharedHandle(&sh) : E_NOINTERFACE;
    if (res) res->Release();
    if (FAILED(hr) || !sh) { LOG("XR: GetSharedHandle hr=%08lx", (unsigned long)hr); return false; }
    hr = g_dev11->OpenSharedResource(sh, __uuidof(ID3D11Texture2D), (void**)&s.tex11);
    if (FAILED(hr)) { LOG("XR: D3D11 OpenSharedResource hr=%08lx", (unsigned long)hr); return false; }
    if (!g_evt10) {
        D3D10_QUERY_DESC qd{ D3D10_QUERY_EVENT, 0 };
        g_dev10->CreateQuery(&qd, &g_evt10);
    }
    return true;
}

// Make the D3D10 copies visible to D3D11: flush the game device and wait for its GPU work to finish.
static void Sync10() {
    if (!g_evt10) { g_dev10->Flush(); return; }
    g_evt10->End();
    g_dev10->Flush();
    BOOL done = FALSE; DWORD t0 = GetTickCount();
    while (g_evt10->GetData(&done, sizeof(done), 0) == S_FALSE) {
        if (GetTickCount() - t0 > 100) break;
        YieldProcessor();
    }
}

static bool CreateChain(Chain& c, int32_t w, int32_t h, int64_t fmt) {
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (!Check(p_xrCreateSwapchain(g_sess, &ci, &c.sc), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    p_xrEnumerateSwapchainImages(c.sc, 0, &n, nullptr);
    c.images.assign(n, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    p_xrEnumerateSwapchainImages(c.sc, n, &n, (XrSwapchainImageBaseHeader*)c.images.data());
    c.w = w; c.h = h;
    return true;
}

// ---------------------------------------------------------------- controller input
// One action set; per-hand subaction paths for the shared actions (Touch controller profile).
static XrActionSet g_set = XR_NULL_HANDLE;
static XrAction g_aGrip = XR_NULL_HANDLE, g_aAim = XR_NULL_HANDLE, g_aTrig = XR_NULL_HANDLE, g_aSqueeze = XR_NULL_HANDLE,
                g_aStick = XR_NULL_HANDLE, g_aStickClick = XR_NULL_HANDLE, g_aBtn1 = XR_NULL_HANDLE, g_aBtn2 = XR_NULL_HANDLE,
                g_aMenu = XR_NULL_HANDLE, g_aHaptic = XR_NULL_HANDLE;
static XrPath g_hand[2] = { XR_NULL_PATH, XR_NULL_PATH };
static XrSpace g_gripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE }, g_aimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static bool g_inputOk = false, g_profileLogged = false;
static HandState g_hs[2];
// game-thread copy (XInputGetState runs on the game thread)
struct CsInit { CRITICAL_SECTION cs; CsInit() { InitializeCriticalSection(&cs); } };
static CsInit g_inCsHolder; static CRITICAL_SECTION& g_inCs = g_inCsHolder.cs;
static HandState g_snap[2]; static XrPosef g_snapHead{ { 0, 0, 0, 1 }, { 0, 0, 0 } }; static bool g_snapHeadValid = false;
static uint64_t g_snapSeq = 0; static float g_rumble[2] = { 0, 0 };

static XrPath P(const char* s) { XrPath p = XR_NULL_PATH; p_xrStringToPath(g_inst, s, &p); return p; }
static XrAction MakeAction(const char* name, const char* loc, XrActionType t, bool perHand) {
    XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
    strcpy(ci.actionName, name); strcpy(ci.localizedActionName, loc); ci.actionType = t;
    if (perHand) { ci.countSubactionPaths = 2; ci.subactionPaths = g_hand; }
    XrAction a = XR_NULL_HANDLE;
    if (!Check(p_xrCreateAction(g_set, &ci, &a), name)) return XR_NULL_HANDLE;
    return a;
}
static bool InputImpl() {
    g_hand[0] = P("/user/hand/left"); g_hand[1] = P("/user/hand/right");
    XrActionSetCreateInfo si{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(si.actionSetName, "gameplay"); strcpy(si.localizedActionSetName, "Gameplay"); si.priority = 0;
    if (!Check(p_xrCreateActionSet(g_inst, &si, &g_set), "xrCreateActionSet")) return false;
    g_aGrip = MakeAction("grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, true);
    g_aAim = MakeAction("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, true);
    g_aTrig = MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_aSqueeze = MakeAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_aStick = MakeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    g_aStickClick = MakeAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_aBtn1 = MakeAction("button_lower", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_aBtn2 = MakeAction("button_upper", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_aMenu = MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    g_aHaptic = MakeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    if (!g_aGrip || !g_aAim || !g_aTrig || !g_aSqueeze || !g_aStick || !g_aStickClick || !g_aBtn1 || !g_aBtn2 || !g_aMenu || !g_aHaptic) return false;
    std::vector<XrActionSuggestedBinding> b;
    auto add = [&](XrAction a, const char* path) { XrPath p = P(path); if (p) b.push_back({ a, p }); };
    const char* hs[2] = { "/user/hand/left", "/user/hand/right" };
    char buf[128];
    for (int h = 0; h < 2; ++h) {
        snprintf(buf, sizeof buf, "%s/input/grip/pose", hs[h]); add(g_aGrip, buf);
        snprintf(buf, sizeof buf, "%s/input/aim/pose", hs[h]); add(g_aAim, buf);
        snprintf(buf, sizeof buf, "%s/input/trigger/value", hs[h]); add(g_aTrig, buf);
        snprintf(buf, sizeof buf, "%s/input/squeeze/value", hs[h]); add(g_aSqueeze, buf);
        snprintf(buf, sizeof buf, "%s/input/thumbstick", hs[h]); add(g_aStick, buf);
        snprintf(buf, sizeof buf, "%s/input/thumbstick/click", hs[h]); add(g_aStickClick, buf);
        snprintf(buf, sizeof buf, "%s/output/haptic", hs[h]); add(g_aHaptic, buf);
    }
    add(g_aBtn1, "/user/hand/left/input/x/click"); add(g_aBtn2, "/user/hand/left/input/y/click");
    add(g_aBtn1, "/user/hand/right/input/a/click"); add(g_aBtn2, "/user/hand/right/input/b/click");
    add(g_aMenu, "/user/hand/left/input/menu/click");
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = P("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b.data(); sb.countSuggestedBindings = (uint32_t)b.size();
    if (!Check(p_xrSuggestInteractionProfileBindings(g_inst, &sb), "xrSuggestInteractionProfileBindings (touch)")) return false;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ac{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ac.subactionPath = g_hand[h]; ac.poseInActionSpace.orientation.w = 1.0f;
        ac.action = g_aGrip; Check(p_xrCreateActionSpace(g_sess, &ac, &g_gripSpace[h]), "grip space");
        ac.action = g_aAim;  Check(p_xrCreateActionSpace(g_sess, &ac, &g_aimSpace[h]), "aim space");
    }
    XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1; ai.actionSets = &g_set;
    if (!Check(p_xrAttachSessionActionSets(g_sess, &ai), "xrAttachSessionActionSets")) return false;
    LOG("XR: controller input ready (%u bindings)", (unsigned)b.size());
    return true;
}

static float GetF(XrAction a, int h, bool* anyActive) { XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = g_hand[h];
    XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT }; const bool active = p_xrGetActionStateFloat(g_sess,&gi,&st) == XR_SUCCESS && st.isActive;
    *anyActive |= active; return active ? fc2vr::authority::FiniteAxis(st.currentState,0,1) : 0; }
static bool GetB(XrAction a, int h, bool* anyActive) { XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = h >= 0 ? g_hand[h] : XR_NULL_PATH;
    XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN }; const bool active = p_xrGetActionStateBoolean(g_sess,&gi,&st) == XR_SUCCESS && st.isActive;
    *anyActive |= active; return active && st.currentState; }
static XrVector2f GetV(XrAction a, int h, bool* anyActive) { XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = g_hand[h];
    XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F }; const bool active = p_xrGetActionStateVector2f(g_sess,&gi,&st) == XR_SUCCESS && st.isActive;
    *anyActive |= active; return active ? XrVector2f{fc2vr::authority::FiniteAxis(st.currentState.x),fc2vr::authority::FiniteAxis(st.currentState.y)} : XrVector2f{0,0}; }

static bool g_holdLost = true;
static fc2vr::authority::PoseCache<XrPosef> g_poseCache[2][2];
static fc2vr::authority::RearmGate g_buttonGate[2], g_poseGate[2];
static bool g_wasTracked[2]{};
static InputStamp g_stamp;
static uint64_t g_generation = 1, g_handGeneration[2] = {1,1}, g_spaceGeneration = 1;
static std::vector<XrTime> g_referenceChanges;
void SetHoldLostPose(bool on) { g_holdLost = on; }
static void ClearButtons(HandState& s) {
    s.trigger = s.squeeze = 0; s.stick = {0,0};
    s.stickClick = s.btn1 = s.btn2 = s.menu = false;
}
static bool Neutral(const HandState& s) {
    return s.trigger <= 0.15f && s.squeeze <= 0.2f && fabsf(s.stick.x) <= 0.2f && fabsf(s.stick.y) <= 0.2f
        && !s.stickClick && !s.btn1 && !s.btn2 && !s.menu;
}
static bool FinitePosition(const XrPosef& p) {
    return fc2vr::authority::FinitePosition(p);
}
static bool FiniteOrientation(const XrPosef& p) {
    return fc2vr::authority::FiniteOrientation(p);
}
static fc2vr::authority::PoseEvidence Evidence(bool active, bool located, const XrSpaceLocation& l) {
    return {active,
        located && (l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && FinitePosition(l.pose),
        located && (l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && FiniteOrientation(l.pose),
        located && (l.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0,
        located && (l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0};
}
static void PublishInput(bool synced, const XrPosef& head, bool headValid, XrTime predictedTime = 0) {
    EnterCriticalSection(&g_inCs);
    g_stamp.sequence = ++g_snapSeq;
    g_stamp.receivedMs = GetTickCount64();
    g_stamp.generation = g_generation;
    g_stamp.focused = g_state == XR_SESSION_STATE_FOCUSED;
    g_stamp.synced = synced;
    g_stamp.predictedTime = predictedTime;
    for (int h = 0; h < 2; ++h) {
        const bool tracked = headValid && g_hs[h].gripEvidence.Tracked() && g_hs[h].aimEvidence.Tracked();
        if (g_wasTracked[h] && !tracked) ++g_handGeneration[h];
        g_wasTracked[h] = tracked;
        g_stamp.handGeneration[h] = g_handGeneration[h];
        g_snap[h] = g_hs[h];
    }
    g_snapHeadValid = headValid && synced && g_stamp.focused;
    g_snapHead = g_snapHeadValid ? head : XrPosef{{0,0,0,1},{0,0,0}};
    const auto stamp = g_stamp;
    LeaveCriticalSection(&g_inCs);
    static int previousState = -1;
    const int state = (stamp.focused ? 1 : 0) | (stamp.synced ? 2 : 0) | (g_hs[0].poseAuthority ? 4 : 0)
        | (g_hs[1].poseAuthority ? 8 : 0) | (headValid ? 16 : 0);
    if (state != previousState || stamp.sequence % 900 == 0) {
        previousState = state;
        LOG("XR: input-authority-1 seq=%llu gen=%llu focused=%d sync=%d headTracked=%d L(active=%d gripValid=%d gripTracked=%d aimValid=%d aimTracked=%d held=%d authority=%d) R(active=%d gripValid=%d gripTracked=%d aimValid=%d aimTracked=%d held=%d authority=%d)",
            (unsigned long long)stamp.sequence,(unsigned long long)stamp.generation,stamp.focused,synced,headValid,
            g_hs[0].active,g_hs[0].gripEvidence.Valid(),g_hs[0].gripEvidence.Tracked(),g_hs[0].aimEvidence.Valid(),g_hs[0].aimEvidence.Tracked(),g_hs[0].gripHeld||g_hs[0].aimHeld,g_hs[0].poseAuthority,
            g_hs[1].active,g_hs[1].gripEvidence.Valid(),g_hs[1].gripEvidence.Tracked(),g_hs[1].aimEvidence.Valid(),g_hs[1].aimEvidence.Tracked(),g_hs[1].gripHeld||g_hs[1].aimHeld,g_hs[1].poseAuthority);
    }
}
static void InvalidateInput(const char* reason, bool clearPoses = false) {
    for (int h = 0; h < 2; ++h) {
        g_buttonGate[h].Clear(); g_poseGate[h].Clear();
        g_hs[h] = HandState{};
        for (int k = 0; k < 2; ++k) {
            if (clearPoses) g_poseCache[h][k].Clear();
            const auto kept = g_poseCache[h][k].Resolve(XrPosef{}, {}, g_holdLost);
            if (k) { g_hs[h].aim = kept.pose; g_hs[h].aimValid = kept.visualValid; g_hs[h].aimHeld = kept.held; }
            else { g_hs[h].grip = kept.pose; g_hs[h].gripValid = kept.visualValid; g_hs[h].gripHeld = kept.held; }
        }
    }
    EnterCriticalSection(&g_inCs);
    const bool wasCurrent = g_stamp.synced;
    ++g_generation; g_rumble[0] = g_rumble[1] = 0;
    // Revoke the published lease atomically; do not leave an old synced stamp
    // readable between generation advancement and the neutral publication below.
    g_stamp.generation = g_generation; g_stamp.synced = false; g_snapHeadValid = false;
    for (auto& snapshot : g_snap) { snapshot.active = snapshot.poseAuthority = false; ClearButtons(snapshot); }
    LeaveCriticalSection(&g_inCs);
    PublishInput(false, {}, false);
    if (wasCurrent) LOG("XR: input authority invalidated (%s)", reason);
}
void InvalidateForRecenter() { InvalidateInput("recenter"); }
InputStamp CurrentInputStamp() {
    EnterCriticalSection(&g_inCs); const InputStamp stamp = g_stamp; LeaveCriticalSection(&g_inCs); return stamp;
}
bool InputCurrent(const InputStamp& source, int hand) {
    EnterCriticalSection(&g_inCs);
    const bool ok = fc2vr::authority::Fresh(source,g_stamp,GetTickCount64(),hand)
        && (hand < 0 || (hand < 2 && g_snapHeadValid && g_snap[hand].poseAuthority));
    LeaveCriticalSection(&g_inCs); return ok;
}
bool InputCurrent() { return InputCurrent(CurrentInputStamp()); }
bool HeadCurrent(const InputStamp& source) {
    EnterCriticalSection(&g_inCs); const auto now = GetTickCount64();
    const bool ok = g_snapHeadValid && fc2vr::authority::Fresh(source,g_stamp,now,0) && fc2vr::authority::Fresh(source,g_stamp,now,1);
    LeaveCriticalSection(&g_inCs); return ok;
}
bool AcceptedInputCurrent(const InputStamp& source, int hand) {
    if (hand < 0 || hand > 1) return false;
    EnterCriticalSection(&g_inCs);
    const bool ok = fc2vr::authority::Fresh(g_stamp,g_stamp,GetTickCount64(),hand)
        && fc2vr::authority::SameContext(source,g_stamp,hand) && g_snapHeadValid && g_snap[hand].poseAuthority;
    LeaveCriticalSection(&g_inCs); return ok;
}
static void RevokeViewPoses() {
    // View location can fail AFTER SyncInput succeeded. Revoke spatial use, not
    // fresh ordinary buttons; changing authority must not renew receipt age.
    for (int h = 0; h < 2; ++h) { g_poseGate[h].Clear(); g_hs[h].poseAuthority = false; }
    EnterCriticalSection(&g_inCs);
    const bool changed = g_snapHeadValid || g_snap[0].poseAuthority || g_snap[1].poseAuthority;
    g_snapHeadValid = false;
    if (changed) g_stamp.sequence = ++g_snapSeq;
    for (int h = 0; h < 2; ++h) {
        if (changed) ++g_handGeneration[h];
        g_stamp.handGeneration[h] = g_handGeneration[h]; g_wasTracked[h] = false; g_snap[h].poseAuthority = false;
    }
    LeaveCriticalSection(&g_inCs);
    static unsigned count = 0;
    if (changed && count++ % 90 == 0) LOG("XR: input-authority-1 spatial use revoked: incomplete view tracking (events=%u, buttons remain fresh)",count);
}

static void SyncInput(XrTime t) {
    bool spaceChanged = false;
    for (auto it = g_referenceChanges.begin(); it != g_referenceChanges.end();) {
        if (t >= *it) { it = g_referenceChanges.erase(it); ++g_spaceGeneration; spaceChanged = true; }
        else ++it;
    }
    if (spaceChanged) InvalidateInput("reference-space effective",true);
    const auto previous = CurrentInputStamp();
    if (previous.synced && !fc2vr::authority::Fresh(previous,previous,GetTickCount64())) InvalidateInput("publication gap");
    if (!g_inputOk || g_state != XR_SESSION_STATE_FOCUSED || !g_referenceChanges.empty()) {
        InvalidateInput("unfocused/pending space"); return;
    }
    XrActiveActionSet as{ g_set, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &as;
    if (p_xrSyncActions(g_sess, &si) != XR_SUCCESS) { InvalidateInput("sync failed"); return; }
    if (!g_profileLogged) {
        XrInteractionProfileState ps{ XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_SUCCEEDED(p_xrGetCurrentInteractionProfile(g_sess, g_hand[1], &ps)) && ps.interactionProfile) {
            char name[256] = ""; uint32_t n = 0; p_xrPathToString(g_inst, ps.interactionProfile, sizeof name, &n, name);
            LOG("XR: interaction profile %s", name); g_profileLogged = true;
        }
    }
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    const bool headLocated = g_view && p_xrLocateSpace(g_view,g_local,t,&hl) == XR_SUCCESS;
    const bool headOk = Evidence(headLocated,headLocated,hl).Tracked();
    for (int h = 0; h < 2; ++h) {
        HandState s;
        for (int k = 0; k < 2; ++k) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = k ? g_aAim : g_aGrip; gi.subactionPath = g_hand[h];
            XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
            const bool active = p_xrGetActionStatePose(g_sess,&gi,&ps) == XR_SUCCESS && ps.isActive;
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
            const bool located = active && p_xrLocateSpace(k ? g_aimSpace[h] : g_gripSpace[h],g_local,t,&l) == XR_SUCCESS;
            const auto evidence = Evidence(active,located,l);
            const auto visual = g_poseCache[h][k].Resolve(l.pose,evidence,g_holdLost);
            if (k) { s.aim = visual.pose; s.aimEvidence = evidence; s.aimValid = visual.visualValid; s.aimHeld = visual.held; }
            else { s.grip = visual.pose; s.gripEvidence = evidence; s.gripValid = visual.visualValid; s.gripHeld = visual.held; }
            s.active |= active;
        }
        // Buttons are independent of positional tracking and even pose-action activity.
        bool buttonsActive = false;
        s.trigger = GetF(g_aTrig,h,&buttonsActive); s.squeeze = GetF(g_aSqueeze,h,&buttonsActive); s.stick = GetV(g_aStick,h,&buttonsActive);
        s.stickClick = GetB(g_aStickClick,h,&buttonsActive); s.btn1 = GetB(g_aBtn1,h,&buttonsActive); s.btn2 = GetB(g_aBtn2,h,&buttonsActive);
        s.menu = h == 0 && GetB(g_aMenu,-1,&buttonsActive);
        s.active |= buttonsActive;
        const bool neutral = Neutral(s);
        const bool poseNeutral = s.trigger <= 0.15f && s.squeeze <= 0.2f;   // locomotion sticks need not stop after pose-only loss
        s.poseAuthority = g_poseGate[h].Update(headOk && s.active && s.gripEvidence.Tracked() && s.aimEvidence.Tracked(),poseNeutral);
        if (!g_buttonGate[h].Update(s.active,neutral)) ClearButtons(s);
        g_hs[h] = s;
    }
    PublishInput(true,hl.pose,headOk,t);
    float want[2]; EnterCriticalSection(&g_inCs); want[0] = g_rumble[0]; want[1] = g_rumble[1]; LeaveCriticalSection(&g_inCs);
    static float prev[2]{};
    for (int h = 0; h < 2; ++h) {
        if (want[h] > 0.02f) Haptic(h,want[h],0.1f);
        else if (prev[h] > 0.02f) Haptic(h,0,0);
        prev[h] = want[h];
    }
}
bool InputSnapshot(HandState out[2], XrPosef& head, bool& headValid, InputStamp* stamp) {
    EnterCriticalSection(&g_inCs);
    const bool fresh = fc2vr::authority::Fresh(g_stamp,g_stamp,GetTickCount64());
    if (stamp) *stamp = g_stamp;
    headValid = fresh && g_snapHeadValid;
    head = headValid ? g_snapHead : XrPosef{{0,0,0,1},{0,0,0}};
    for (int h = 0; h < 2; ++h) {
        out[h] = fresh ? g_snap[h] : HandState{};
        // This API is physical input; the render-only Hand() accessor retains visual fallback.
        out[h].gripValid = fresh && out[h].poseAuthority && out[h].gripEvidence.Tracked();
        out[h].aimValid = fresh && out[h].poseAuthority && out[h].aimEvidence.Tracked();
    }
    LeaveCriticalSection(&g_inCs); return fresh;
}
void SetRumble(float left, float right) { EnterCriticalSection(&g_inCs); g_rumble[0] = left; g_rumble[1] = right; LeaveCriticalSection(&g_inCs); }
const HandState& Hand(int side) { return g_hs[side ? 1 : 0]; }
void Haptic(int side, float amplitude, float seconds, float frequency) {
    if (!g_inputOk || !g_running) return;
    if (amplitude > 0 && !InputCurrent()) return;
    XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = g_aHaptic; hi.subactionPath = g_hand[side ? 1 : 0];
    if (amplitude <= 0.0f) { p_xrStopHapticFeedback(g_sess, &hi); return; }
    XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amplitude > 1.0f ? 1.0f : amplitude; v.duration = (XrDuration)(seconds * 1e9); v.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
    p_xrApplyHapticFeedback(g_sess, &hi, (const XrHapticBaseHeader*)&v);
}

// ---------------------------------------------------------------- init
static void Teardown() {
    InvalidateInput("teardown",true); g_referenceChanges.clear(); ++g_spaceGeneration;
    for (int i = 0; i < 5; ++i) { SafeRelease(g_shared[i].tex10); SafeRelease(g_shared[i].tex11); }
    g_hud = Chain{}; g_ret = Chain{}; g_dot = Chain{}; SafeRelease(g_dotTex); g_menu = Chain{}; SafeRelease(g_menuTex); g_ammo = Chain{}; SafeRelease(g_ammoTex); g_vig = Chain{}; SafeRelease(g_vigTex); SafeRelease(g_vigPlainTex); SafeRelease(g_vigMonoTex[0]); SafeRelease(g_vigMonoTex[1]);
    g_eye[0] = Chain{}; g_eye[1] = Chain{}; g_screen = Chain{};
    if (g_inst && p_xrDestroyInstance) p_xrDestroyInstance(g_inst);   // also destroys session/space/swapchains
    g_inst = XR_NULL_HANDLE; g_sess = XR_NULL_HANDLE; g_local = XR_NULL_HANDLE; g_view = XR_NULL_HANDLE; g_sys = XR_NULL_SYSTEM_ID;
    SafeRelease(g_ctx11); SafeRelease(g_dev11);
    g_running = false; g_state = XR_SESSION_STATE_UNKNOWN;
    g_inputOk = false; g_set = XR_NULL_HANDLE; for (int h = 0; h < 2; ++h) { g_gripSpace[h] = g_aimSpace[h] = XR_NULL_HANDLE; g_hs[h] = HandState{}; }
}

static bool InstanceImpl(ID3D10Device* dev10) {
    g_dev10 = dev10;
    if (!LoadLoader()) return false;
    const char* exts[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, nullptr };
    uint32_t nExt = 1;
    {   // v230: log what the runtime offers; enable XR_FB_display_refresh_rate when present (read / request the refresh rate)
        PFN_xrEnumerateInstanceExtensionProperties enumExt = nullptr;
        p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExt);
        uint32_t n = 0; g_hasRefreshExt = false;
        if (enumExt && XR_SUCCEEDED(enumExt(nullptr, 0, &n, nullptr)) && n) {
            std::vector<XrExtensionProperties> props(n, XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES });
            enumExt(nullptr, n, &n, props.data());
            static char all[4096]; size_t len = 0; all[0] = 0;
            for (auto& e : props) {
                if (len + strlen(e.extensionName) + 2 < sizeof all) len += snprintf(all + len, sizeof all - len, "%s ", e.extensionName);
                if (!strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) g_hasRefreshExt = true;
            }
            LOG("XR: %u runtime extensions: %s", n, all);
        }
        if (g_hasRefreshExt) exts[nExt++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
        char ip[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA("dinput8.dll"), ip, MAX_PATH);
        char* sl = strrchr(ip, '\\'); if (sl) strcpy(sl + 1, "fc2vr.ini");
        char buf[32] = ""; GetPrivateProfileStringA("VR", "DisplayRefreshRate", "0", buf, sizeof buf, ip); g_wantRefresh = (float)atof(buf);
    }
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "FarCry2VR");
    strcpy(ici.applicationInfo.engineName, "Dunia");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = nExt; ici.enabledExtensionNames = exts;
    {   XrResult cr = p_xrCreateInstance(&ici, &g_inst);
        if (XR_FAILED(cr)) {   // v231: say which runtime this 32-bit game was given and whether it can load here
            static bool told = false;
            if (!told) {
                told = true;
                std::wstring src; xrprobe::Runtime a = xrprobe::Active32(&src), b = xrprobe::Active64();
                LOG("XR: runtime for this 32-bit game (%s): %s -> %s (%s)", xrprobe::N(src).c_str(), a.json.empty() ? "(none)" : xrprobe::N(a.json).c_str(),
                    a.library.empty() ? "(no library)" : xrprobe::N(a.library).c_str(), xrprobe::BitsText(a.bits));
                LOG("XR: 64-bit active runtime: %s (%s)", b.json.empty() ? "(none)" : xrprobe::N(b.json).c_str(), xrprobe::BitsText(b.bits));
                for (auto& r : xrprobe::Installed()) LOG("XR: installed runtime %s: %s", xrprobe::N(r.json).c_str(), xrprobe::BitsText(r.bits));
                if (cr == XR_ERROR_RUNTIME_UNAVAILABLE || a.bits != xrprobe::Bits32)
                    LOG("XR: Far Cry 2 is a 32-bit game and needs an OpenXR runtime with 32-bit support (for example Virtual Desktop, or SteamVR 2.17 or later set as the OpenXR runtime). Pick one under Launcher > Display > OpenXR runtime.");
            }
        }
        if (!Check(cr, "xrCreateInstance")) return false; }
    if (!LoadInstanceFuncs()) { snprintf(g_err, sizeof(g_err), "function load failed"); return false; }
    o_xrWaitFrame = p_xrWaitFrame; p_xrWaitFrame = W_xrWaitFrame;
    o_xrEndFrame = p_xrEndFrame; p_xrEndFrame = W_xrEndFrame;
    if (g_hasRefreshExt) {
        p_xrGetInstanceProcAddr(g_inst, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&p_xrEnumerateDisplayRefreshRatesFB);
        p_xrGetInstanceProcAddr(g_inst, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&p_xrGetDisplayRefreshRateFB);
        p_xrGetInstanceProcAddr(g_inst, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&p_xrRequestDisplayRefreshRateFB);
    }
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    p_xrGetInstanceProperties(g_inst, &ip);
    LOG("XR: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(p_xrGetSystem(g_inst, &sgi, &g_sys), "xrGetSystem")) return false;   // headset absent: retryable
    g_fatal = true;   // from here on, failures are not fixed by retrying; avoid session churn
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    p_xrGetSystemProperties(g_inst, g_sys, &sp);
    LOG("XR: system '%s'", sp.systemName);
    uint32_t nv = 0; XrViewConfigurationView vcv[2]{ { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    p_xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv);
    g_recW = vcv[0].recommendedImageRectWidth; g_recH = vcv[0].recommendedImageRectHeight;
    LOG("XR: recommended eye %ux%u", g_recW, g_recH);
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (!Check(p_xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req), "xrGetD3D11GraphicsRequirementsKHR")) return false;
    if (!CreateD3D11(req.adapterLuid)) return false;
    XrGraphicsBindingD3D11KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = g_dev11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = g_sys;
    if (!Check(p_xrCreateSession(g_inst, &sci, &g_sess), "xrCreateSession")) return false;
    LogRefreshRates("at session start");
    if (g_wantRefresh > 0 && p_xrRequestDisplayRefreshRateFB) {
        XrResult rr = p_xrRequestDisplayRefreshRateFB(g_sess, g_wantRefresh);
        LOG("XR: DisplayRefreshRate=%.0f requested -> %d", g_wantRefresh, (int)rr);
    }
    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(p_xrCreateReferenceSpace(g_sess, &rs, &g_local), "xrCreateReferenceSpace")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (p_xrCreateReferenceSpace(g_sess, &rs, &g_view) != XR_SUCCESS) { g_view = XR_NULL_HANDLE; LOG("XR: no VIEW space"); }
    g_inputOk = InputImpl();
    if (!g_inputOk) LOG("XR: controller input unavailable (%s)", g_err);
    return true;
}

// Pick a swapchain format in the source's typeless family, preferring the sRGB variant
// (the game's output is already gamma-encoded, so an sRGB swapchain displays it correctly).
static int64_t ChooseFormat(DXGI_FORMAT src) {
    uint32_t nf = 0; p_xrEnumerateSwapchainFormats(g_sess, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf); p_xrEnumerateSwapchainFormats(g_sess, nf, &nf, fmts.data());
    DXGI_FORMAT want[2] = { SrgbOf(src), src };
    for (DXGI_FORMAT w : want) for (int64_t f : fmts) if (f == (int64_t)w) return f;
    for (int64_t f : fmts) if (TypelessFamily((DXGI_FORMAT)f) == TypelessFamily(src)) return f;
    return 0;
}

static bool SwapchainsImpl(const D3D10_TEXTURE2D_DESC& eye, const D3D10_TEXTURE2D_DESC& bb) {
    g_eyeW = eye.Width; g_eyeH = eye.Height; g_eyeFormat = eye.Format;
    g_w = bb.Width; g_h = bb.Height; g_bbFormat = bb.Format; g_bbMsaa = bb.SampleDesc.Count > 1;
    LOG("XR: eye targets %ux%u fmt=%d, backbuffer %ux%u fmt=%d samples=%u", g_eyeW, g_eyeH, (int)g_eyeFormat,
        g_w, g_h, (int)g_bbFormat, bb.SampleDesc.Count);
    int64_t fe = ChooseFormat(g_eyeFormat), fs = ChooseFormat(g_bbFormat);
    if (!fe || !fs) { snprintf(g_err, sizeof(g_err), "no compatible swapchain format (eye %d, screen %d)", (int)g_eyeFormat, (int)g_bbFormat); LOG("XR: %s", g_err); return false; }
    LOG("XR: swapchain formats eye=%lld screen=%lld", (long long)fe, (long long)fs);
    g_eyeChainFmt = (DXGI_FORMAT)fe;
    {   const int32_t cw = (int32_t)(g_outW ? g_outW : g_eyeW), ch = (int32_t)(g_outH ? g_outH : g_eyeH);
        if (g_outW) LOG("XR: DLSS upscaling: eyes render %ux%u, shown %dx%d", g_eyeW, g_eyeH, cw, ch);
        for (int e = 0; e < 2; ++e) if (!CreateChain(g_eye[e], cw, ch, fe)) return false; }
    if (!CreateChain(g_screen, (int32_t)g_w, (int32_t)g_h, fs)) return false;
    for (int i = 0; i < 2; ++i) if (!CreateShared(g_shared[i], g_eyeW, g_eyeH, g_eyeFormat)) { snprintf(g_err, sizeof(g_err), "shared eye texture %d failed", i); return false; }
    if (!CreateShared(g_shared[2], g_w, g_h, g_bbFormat)) { snprintf(g_err, sizeof(g_err), "shared screen texture failed"); return false; }
    if (CreateChain(g_hud, (int32_t)g_eyeW, (int32_t)g_eyeH, fe) && CreateShared(g_shared[3], g_eyeW, g_eyeH, g_eyeFormat)) LOG("XR: HUD layer %ux%u ready", g_eyeW, g_eyeH);
    else { LOG("XR: HUD layer unavailable"); SafeRelease(g_shared[3].tex10); SafeRelease(g_shared[3].tex11); }
    if (CreateChain(g_ret, (int32_t)g_eyeW, (int32_t)g_eyeH, fe) && CreateShared(g_shared[4], g_eyeW, g_eyeH, g_eyeFormat)) LOG("XR: reticle layer ready");
    if (CreateChain(g_ammo, AmmoW, AmmoH, fe)) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = AmmoW; td.Height = AmmoH; td.MipLevels = td.ArraySize = 1; td.Format = (DXGI_FORMAT)fe;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev11->CreateTexture2D(&td, nullptr, &g_ammoTex))) g_ammoTex = nullptr;
    }
    if (CreateChain(g_menu, MenuW, MenuH, fe)) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = MenuW; td.Height = MenuH; td.MipLevels = td.ArraySize = 1; td.Format = (DXGI_FORMAT)fe;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev11->CreateTexture2D(&td, nullptr, &g_menuTex))) g_menuTex = nullptr;
    }
    if (CreateChain(g_dot, 64, 64, fe)) {   // white dot with a dark rim, premultiplied alpha
        static uint32_t px[64 * 64];
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            float dx = x + 0.5f - 32, dy = y + 0.5f - 32, r = sqrtf(dx * dx + dy * dy);
            float a = r < 22 ? 1.0f : r < 30 ? 1.0f - (r - 22) / 8 : 0.0f;    // body + soft edge
            float c = r < 18 ? 1.0f : r < 22 ? 1.0f - (r - 18) / 4 * 0.8f : 0.2f;   // white centre, dark rim
            uint8_t A = (uint8_t)(a * 255), C = (uint8_t)(a * c * 255);
            px[y * 64 + x] = (uint32_t)C | ((uint32_t)C << 8) | ((uint32_t)C << 16) | ((uint32_t)A << 24);
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = (DXGI_FORMAT)fe;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ px, 64 * 4, 0 };
        if (SUCCEEDED(g_dev11->CreateTexture2D(&td, &sd, &g_dotTex))) LOG("XR: aiming dot layer ready"); else g_dotTex = nullptr;
    }
    if (CreateChain(g_vig, 1024, 1024, fe)) {   // black outside a circle, crosshair inside (premultiplied alpha)
        const int N = 1024; const float R = N * 0.5f - 6;
        std::vector<uint32_t> px((size_t)N * N);
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            float dx = x + 0.5f - N * 0.5f, dy = y + 0.5f - N * 0.5f, r = sqrtf(dx * dx + dy * dy);
            float a = r > R ? 1.0f : r > R - 10 ? (r - (R - 10)) / 10 : 0.0f;          // soft rim
            float adx = fabsf(dx), ady = fabsf(dy);
            if (g_scopeCross && r < R) {   // sniper reticle: fine cross, heavy posts from the rim, mil dots
                auto cov = [](float d, float half) { return fminf(1.0f, fmaxf(0.0f, half + 0.5f - d)); };   // antialiased band
                float c = fmaxf(cov(adx, 1.0f), cov(ady, 1.0f));
                if (adx > R * 0.5f) c = fmaxf(c, cov(ady, 4.0f));
                if (ady > R * 0.5f) c = fmaxf(c, cov(adx, 4.0f));
                if (g_scopeCross >= 2) {
                    const float step = R * 0.08f;
                    for (int k = 1; k <= 5; ++k) {
                        float o = step * k;
                        c = fmaxf(c, cov(sqrtf((adx - o) * (adx - o) + ady * ady), 3.5f));
                        c = fmaxf(c, cov(sqrtf((ady - o) * (ady - o) + adx * adx), 3.5f));
                    }
                }
                a = fmaxf(a, c * 0.92f);
            }
            uint8_t A = (uint8_t)(fminf(1.0f, a) * 255);
            px[(size_t)y * N + x] = (uint32_t)A << 24;   // black, premultiplied
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = N; td.MipLevels = td.ArraySize = 1; td.Format = (DXGI_FORMAT)fe;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ px.data(), N * 4, 0 };
        if (SUCCEEDED(g_dev11->CreateTexture2D(&td, &sd, &g_vigTex))) LOG("XR: scope window layer ready"); else g_vigTex = nullptr;
        // v214: the monocular's window - the round rim only (the game draws its own targeting ring)
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            float dx = x + 0.5f - N * 0.5f, dy = y + 0.5f - N * 0.5f, r = sqrtf(dx * dx + dy * dy);
            float a = r > R ? 1.0f : r > R - 10 ? (r - (R - 10)) / 10 : 0.0f;
            px[(size_t)y * N + x] = (uint32_t)(uint8_t)(fminf(1.0f, a) * 255) << 24;
        }
        if (FAILED(g_dev11->CreateTexture2D(&td, &sd, &g_vigPlainTex))) g_vigPlainTex = nullptr;
        // v217: the monocular's targeting ring (red; green with something taggable in it), drawn by the mod in the window
        for (int k = 0; k < 2; ++k) {
            const float rr = R * g_monoRing, half = 3.0f;   // v218: ring radius (MonocularRingSize, part of the window radius)   // ring radius and half thickness (px of 1024)
            const float col[2][3] = { { 0.95f, 0.12f, 0.08f }, { 0.20f, 0.95f, 0.25f } };
            for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
                float dx = x + 0.5f - N * 0.5f, dy = y + 0.5f - N * 0.5f, r = sqrtf(dx * dx + dy * dy);
                float a = r > R ? 1.0f : r > R - 10 ? (r - (R - 10)) / 10 : 0.0f;   // rim (black)
                float c = fminf(1.0f, fmaxf(0.0f, half + 0.5f - fabsf(r - rr)));     // ring (antialiased)
                float A = fminf(1.0f, a + c * 0.9f * (1.0f - a));
                float cr = col[k][0] * c * 0.9f, cg = col[k][1] * c * 0.9f, cb = col[k][2] * c * 0.9f;   // premultiplied
                px[(size_t)y * N + x] = (uint32_t)(uint8_t)(cr * 255) | ((uint32_t)(uint8_t)(cg * 255) << 8) | ((uint32_t)(uint8_t)(cb * 255) << 16) | ((uint32_t)(uint8_t)(A * 255) << 24);
            }
            if (FAILED(g_dev11->CreateTexture2D(&td, &sd, &g_vigMonoTex[k]))) g_vigMonoTex[k] = nullptr;
        }
    }
    else { LOG("XR: reticle layer unavailable"); SafeRelease(g_shared[4].tex10); SafeRelease(g_shared[4].tex11); }
    g_ready = true; g_fatal = false;
    LOG("XR: ready");
    return true;
}

ID3D10Texture2D* HudTexture() { return g_shared[3].tex10; }
void SetAmmoImage(const uint32_t* px) { if (px) { memcpy(g_ammoPx, px, sizeof g_ammoPx); g_ammoDirty = true; g_ammoHave = true; } }
void SetMenu(const uint32_t* px, const XrPosef& pose, float width) {
    if (!g_menu.sc || !g_menuTex) return;
    if (px) { memcpy(g_menuPx, px, sizeof g_menuPx); g_menuDirty = true; }
    g_menuOn = true; g_menuPose = pose; g_menuWidth = width;
}
void SetDotLayer(const XrPosef& pose, float size) { if (g_dot.sc && g_dotTex) { g_dotOn = true; g_dotPose = pose; g_dotSize = size; } }
ID3D10Texture2D* ReticleTexture() { return g_shared[4].tex10; }
void SetReticleLayer(const XrPosef& pose, float width, float frac) { if (g_ret.sc && g_shared[4].tex11) { g_retOn = true; g_retPose = pose; g_retWidth = width; g_retFrac = frac; } }
void SetHudLayer(const XrPosef& pose, float width, bool viewSpace) { if (g_hud.sc && g_shared[3].tex11) { g_hudOn = true; g_hudPose = pose; g_hudWidth = width; g_hudView = viewSpace && g_view; } }

bool InitInstance(ID3D10Device* dev10, uint32_t& recW, uint32_t& recH) {
    if (g_sess) { recW = g_recW; recH = g_recH; return true; }
    if (InstanceImpl(dev10)) { recW = g_recW; recH = g_recH; return true; }
    Teardown();
    return false;
}

bool InitSwapchains(const D3D10_TEXTURE2D_DESC& eye, const D3D10_TEXTURE2D_DESC& bb) {
    if (g_ready) return true;
    if (SwapchainsImpl(eye, bb)) return true;
    Teardown();
    return false;
}

// ---------------------------------------------------------------- events
void PollEvents() {
    if (!g_inst) return;
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (p_xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { g_running = false; InvalidateInput("instance loss",true); }
        if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            auto* d = (XrEventDataDisplayRefreshRateChangedFB*)&ev;
            LOG("XR: display refresh changed %.1f -> %.1f Hz", d->fromDisplayRefreshRate, d->toDisplayRefreshRate);
        }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            if (s->session != g_sess) { ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER}; continue; }
            g_state = s->state;
            if (g_state != XR_SESSION_STATE_FOCUSED) InvalidateInput("session state");
            LOG("XR: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(p_xrBeginSession(g_sess, &bi), "xrBeginSession")) g_running = true;
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(g_sess); g_running = false;
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
            }
        }
        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto* change = (const XrEventDataReferenceSpaceChangePending*)&ev;
            if (change->session == g_sess && change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                g_referenceChanges.push_back(change->changeTime);
                InvalidateInput("reference-space pending");
                LOG("XR: LOCAL reference change queued at %lld", (long long)change->changeTime);
            }
        }
        ev = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------- frames
static bool WaitBegin(XrFrameState& fs) {
    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
    if (!Check(p_xrWaitFrame(g_sess, &wi, &fs), "xrWaitFrame")) { InvalidateInput("wait failed"); return false; }
    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r = p_xrBeginFrame(g_sess, &bi);
    if (!Check(r, "xrBeginFrame")) { InvalidateInput("begin failed"); return false; }
    g_frameBegun = true;
    diag::GpuFrameStart(g_dev10);
    SyncInput(fs.predictedDisplayTime);
    return true;
}

bool BeginFrame(FrameViews& out) {
    out = FrameViews{};
    if (!SessionRunning() || g_frameBegun) return false;
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (!WaitBegin(fs)) return false;
    out.displayTime = fs.predictedDisplayTime;
    out.spaceGeneration = g_spaceGeneration;
    out.shouldRender = fs.shouldRender == XR_TRUE;
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime; li.space = g_local;
    XrViewState vs{ XR_TYPE_VIEW_STATE }; uint32_t n = 0;
    out.views[0].type = out.views[1].type = XR_TYPE_VIEW;
    if (XR_SUCCEEDED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, out.views)) && n == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && FiniteOrientation(out.views[0].pose) && FiniteOrientation(out.views[1].pose))
        out.valid = true;
    out.positionValid = out.valid && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
        && FinitePosition(out.views[0].pose) && FinitePosition(out.views[1].pose);
    const XrViewStateFlags trackedViews = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT
        | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    if (!out.positionValid || (vs.viewStateFlags & trackedViews) != trackedViews) RevokeViewPoses();
    return true;
}

static void CopyInto(Shared& s, ID3D10Texture2D* src, bool msaa, DXGI_FORMAT fmt) {
    if (msaa) g_dev10->ResolveSubresource(s.tex10, 0, src, 0, fmt);
    else      g_dev10->CopyResource(s.tex10, src);
}

void CopyEye(int eye, ID3D10Texture2D* src) { if (g_ready) CopyInto(g_shared[eye], src, false, g_eyeFormat); }

static void DrawArc(ID3D11Texture2D* tex, int e, const XrRect2Di& rect);
static bool BlitStatic(Chain& c, ID3D11Texture2D* src, int arcEye = -1, const XrRect2Di* arcRect = nullptr) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(p_xrAcquireSwapchainImage(c.sc, &ai, &idx), "xrAcquireSwapchainImage(dot)")) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    bool ok = Check(p_xrWaitSwapchainImage(c.sc, &wi), "xrWaitSwapchainImage(dot)");
    if (ok) { g_ctx11->CopyResource(c.images[idx].texture, src); if (arcEye >= 0 && arcRect) DrawArc(c.images[idx].texture, arcEye, *arcRect); }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(c.sc, &ri);
    return ok;
}
static bool BlitToChain(Chain& c, Shared& s, int arcEye = -1, const XrRect2Di* arcRect = nullptr) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(p_xrAcquireSwapchainImage(c.sc, &ai, &idx), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000; // 100 ms
    bool ok = Check(p_xrWaitSwapchainImage(c.sc, &wi), "xrWaitSwapchainImage");
    if (ok) { g_ctx11->CopyResource(c.images[idx].texture, s.tex11); if (arcEye >= 0 && arcRect) DrawArc(c.images[idx].texture, arcEye, *arcRect); }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(c.sc, &ri);
    return ok;
}


// ---------------------------------------------------------------- throwing arc overlay (drawn into the eye images)
static XrVector3f g_arcPts[96]; static int g_arcN = 0; static bool g_arcLand = false, g_arcOk = true; static XrVector3f g_arcNrm{ 0, 1, 0 };
static bool g_arcFrame = false; static XrView g_arcViews[2];
void SetArc(const XrVector3f* pts, int n, bool land, const XrVector3f& normal, bool ok) {
    if (n > 96) n = 96;
    if (n < 2 || !pts) { g_arcN = 0; return; }
    memcpy(g_arcPts, pts, sizeof(XrVector3f) * n); g_arcN = n; g_arcLand = land; g_arcNrm = normal; g_arcOk = ok; g_arcFrame = true;
}
static ID3D11VertexShader* g_arcVS = nullptr; static ID3D11PixelShader* g_arcPS = nullptr; static ID3D11InputLayout* g_arcIL = nullptr;
static ID3D11Buffer* g_arcVB = nullptr; static ID3D11BlendState* g_arcBS = nullptr; static ID3D11RasterizerState* g_arcRS = nullptr;
static ID3D11DepthStencilState* g_arcDS = nullptr; static bool g_arcInitTried = false, g_arcReady = false;
struct ArcRtv { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; };
static std::vector<ArcRtv> g_arcRtvs;
static const int ArcMaxV = 6144;
struct ArcVtx { float x, y, z, w, r, g, b, a; };
static bool ArcInit() {
    if (g_arcInitTried) return g_arcReady;
    g_arcInitTried = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!comp) { LOG("XR: throwing arc needs d3dcompiler_47.dll - not available"); return false; }
    static const char* src =
        "struct V { float4 p : POSITION; float4 c : COLOR; }; struct P { float4 p : SV_Position; float4 c : COLOR; };\n"
        "P vs(V v) { P o; o.p = v.p; o.c = v.c; return o; }\n"
        "float4 ps(P i) : SV_Target { return i.c; }\n";
    ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(comp(src, strlen(src), "arc", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bv, &err)) &&
              SUCCEEDED(comp(src, strlen(src), "arc", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bp, &err));
    if (!ok) { LOG("XR: throwing arc shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); if (bv) bv->Release(); if (bp) bp->Release(); return false; }
    D3D11_INPUT_ELEMENT_DESC il[2] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                                       { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ok = SUCCEEDED(g_dev11->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_arcVS)) &&
         SUCCEEDED(g_dev11->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_arcPS)) &&
         SUCCEEDED(g_dev11->CreateInputLayout(il, 2, bv->GetBufferPointer(), bv->GetBufferSize(), &g_arcIL));
    bv->Release(); bp->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(ArcVtx) * ArcMaxV; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(g_dev11->CreateBuffer(&bd, nullptr, &g_arcVB));
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(g_dev11->CreateBlendState(&bl, &g_arcBS));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE;
    ok = ok && SUCCEEDED(g_dev11->CreateRasterizerState(&rd, &g_arcRS));
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(g_dev11->CreateDepthStencilState(&dd, &g_arcDS));
    g_arcReady = ok;
    LOG("XR: throwing arc overlay %s", ok ? "ready" : "failed");
    return ok;
}
static ID3D11RenderTargetView* ArcRtvFor(ID3D11Texture2D* t) {
    for (auto& r : g_arcRtvs) if (r.tex == t) return r.rtv;
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = g_eyeChainFmt; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* v = nullptr;
    if (FAILED(g_dev11->CreateRenderTargetView(t, &rv, &v))) v = nullptr;
    g_arcRtvs.push_back({ t, v });
    return v;
}
static XrVector3f QRotX(const XrQuaternionf& q, const XrVector3f& v) {   // rotate v by q
    float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx) };
}
// Draws the arc into eye e's swapchain image (rect = the part of the image the eye shows).
static void DrawArc(ID3D11Texture2D* tex, int e, const XrRect2Di& rect) {
    if (!g_arcFrame || g_arcN < 2 || !ArcInit()) return;
    ID3D11RenderTargetView* rtv = ArcRtvFor(tex);
    if (!rtv) return;
    const XrView& v = g_arcViews[e];
    XrQuaternionf qi{ -v.pose.orientation.x, -v.pose.orientation.y, -v.pose.orientation.z, v.pose.orientation.w };
    const float tl = tanf(v.fov.angleLeft), tr = tanf(v.fov.angleRight), tu = tanf(v.fov.angleUp), td = tanf(v.fov.angleDown);
    const float pxW = (float)rect.extent.width, pxH = (float)rect.extent.height;
    auto toView = [&](const XrVector3f& p) { XrVector3f d{ p.x - v.pose.position.x, p.y - v.pose.position.y, p.z - v.pose.position.z }; return QRotX(qi, d); };
    auto ndc = [&](const XrVector3f& q, float& x, float& y) { float iz = 1.0f / -q.z; x = (2 * q.x * iz - (tr + tl)) / (tr - tl); y = (2 * q.y * iz - (tu + td)) / (tu - td); };
    static ArcVtx vb[ArcMaxV]; int nv = 0;
    const float cr = 1.0f, cg = 1.0f, cb = 1.0f;   // white (dimmer without a grenade / molotov)
    const float okA = g_arcOk ? 1.0f : 0.45f;
    auto quad = [&](float x0, float y0, float x1, float y1, float hw0, float hw1, float a0, float a1) {   // thick segment in NDC
        if (nv + 6 > ArcMaxV) return;
        float dx = (x1 - x0) * pxW, dy = (y1 - y0) * pxH, l = sqrtf(dx * dx + dy * dy); if (l < 1e-4f) return;
        float nx = -dy / l, ny = dx / l;   // pixel-space normal
        float ox0 = nx * hw0 * 2 / pxW, oy0 = ny * hw0 * 2 / pxH, ox1 = nx * hw1 * 2 / pxW, oy1 = ny * hw1 * 2 / pxH;
        ArcVtx a{ x0 - ox0, y0 - oy0, 0.5f, 1, cr, cg, cb, a0 }, b{ x0 + ox0, y0 + oy0, 0.5f, 1, cr, cg, cb, a0 },
               c{ x1 - ox1, y1 - oy1, 0.5f, 1, cr, cg, cb, a1 }, d{ x1 + ox1, y1 + oy1, 0.5f, 1, cr, cg, cb, a1 };
        vb[nv++] = a; vb[nv++] = b; vb[nv++] = c; vb[nv++] = b; vb[nv++] = d; vb[nv++] = c;
    };
    const float focal = pxW / (tr - tl);   // pixels per unit of tan
    const float thick = 0.012f;           // arc half-width in metres
    // dashed arc: every other segment, fading in from the hand
    for (int i = 0; i + 1 < g_arcN; ++i) {
        if ((i / 2) % 2) continue;
        XrVector3f a = toView(g_arcPts[i]), b = toView(g_arcPts[i + 1]);
        if (a.z > -0.05f || b.z > -0.05f) continue;
        float x0, y0, x1, y1; ndc(a, x0, y0); ndc(b, x1, y1);
        float hw0 = fmaxf(1.0f, thick * focal / -a.z), hw1 = fmaxf(1.0f, thick * focal / -b.z);
        float f0 = fminf(1.0f, (float)i / 4.0f) * 0.85f, f1 = fminf(1.0f, (float)(i + 1) / 4.0f) * 0.85f;
        quad(x0, y0, x1, y1, hw0, hw1, f0 * okA, f1 * okA);
    }
    // landing ring on the surface
    if (g_arcLand) {
        XrVector3f c = g_arcPts[g_arcN - 1], n = g_arcNrm;
        float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z); if (nl < 1e-4f) n = { 0, 1, 0 }; else n = { n.x / nl, n.y / nl, n.z / nl };
        XrVector3f t = fabsf(n.y) < 0.9f ? XrVector3f{ 0, 1, 0 } : XrVector3f{ 1, 0, 0 };
        XrVector3f u{ n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x };
        float ul = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z); u = { u.x / ul, u.y / ul, u.z / ul };
        XrVector3f w{ n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x };
        const float R = 0.6f; const int K = 32;
        for (int k = 0; k < K; ++k) {
            float a0 = 6.2831853f * k / K, a1 = 6.2831853f * (k + 1) / K;
            XrVector3f p0{ c.x + (u.x * cosf(a0) + w.x * sinf(a0)) * R + n.x * 0.03f, c.y + (u.y * cosf(a0) + w.y * sinf(a0)) * R + n.y * 0.03f, c.z + (u.z * cosf(a0) + w.z * sinf(a0)) * R + n.z * 0.03f };
            XrVector3f p1{ c.x + (u.x * cosf(a1) + w.x * sinf(a1)) * R + n.x * 0.03f, c.y + (u.y * cosf(a1) + w.y * sinf(a1)) * R + n.y * 0.03f, c.z + (u.z * cosf(a1) + w.z * sinf(a1)) * R + n.z * 0.03f };
            XrVector3f a = toView(p0), b = toView(p1);
            if (a.z > -0.05f || b.z > -0.05f) continue;
            float x0, y0, x1, y1; ndc(a, x0, y0); ndc(b, x1, y1);
            quad(x0, y0, x1, y1, fmaxf(1.0f, 0.025f * focal / -a.z), fmaxf(1.0f, 0.025f * focal / -b.z), 0.9f * okA, 0.9f * okA);
        }
    }
    if (!nv) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx11->Map(g_arcVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, vb, sizeof(ArcVtx) * nv); g_ctx11->Unmap(g_arcVB, 0);
    D3D11_VIEWPORT vp{ (float)rect.offset.x, (float)rect.offset.y, pxW, pxH, 0, 1 };
    UINT stride = sizeof(ArcVtx), off = 0; float bf[4] = { 0, 0, 0, 0 };
    g_ctx11->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx11->OMSetBlendState(g_arcBS, bf, 0xFFFFFFFF); g_ctx11->OMSetDepthStencilState(g_arcDS, 0);
    g_ctx11->RSSetState(g_arcRS); g_ctx11->RSSetViewports(1, &vp);
    g_ctx11->IASetInputLayout(g_arcIL); g_ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx11->IASetVertexBuffers(0, 1, &g_arcVB, &stride, &off);
    g_ctx11->VSSetShader(g_arcVS, nullptr, 0); g_ctx11->PSSetShader(g_arcPS, nullptr, 0);
    g_ctx11->GSSetShader(nullptr, nullptr, 0);
    g_ctx11->Draw(nv, 0);
    ID3D11RenderTargetView* none = nullptr; g_ctx11->OMSetRenderTargets(1, &none, nullptr);
}
static EyeFilter g_eyeFilter = nullptr;
void SetEyeFilter(EyeFilter f) { g_eyeFilter = f; }
// the eye image, or what the filter (DLSS) made of it
static bool g_copyBack = false; static ID3D11Query* g_copyQ = nullptr; static bool g_copyPending = false;
void SetCopyBack(bool on) { g_copyBack = on; }
ID3D10Texture2D* EyeTexture10(int e) { return (e == 0 || e == 1) ? g_shared[e].tex10 : nullptr; }
// the D3D10 device may read the eye textures again once the D3D11 copy-backs of the last frame are done
void WaitEyeCopies() {
    if (!g_copyPending || !g_copyQ) return;
    DWORD t0 = GetTickCount();
    while (g_ctx11->GetData(g_copyQ, nullptr, 0, 0) == S_FALSE) { if (GetTickCount() - t0 > 20) break; YieldProcessor(); }
    g_copyPending = false;
}
static XrRect2Di g_arcRect[2];
void SetEyeOutputSize(UINT w, UINT h) { g_outW = w; g_outH = h; }
// rect: the eye's shown rect in swapchain (output) pixels; out: the rect actually used for this frame's image
static bool BlitEye(int e, const XrRect2Di& rect, XrRect2Di& out) {
    out = rect;
    if (g_eyeFilter) if (ID3D11Texture2D* t = g_eyeFilter(g_dev11, g_ctx11, e, g_shared[e].tex11)) {
        // AFW: the eye's shared texture holds what it showed (the DLSS result), the source for rebuilding it next frame
        if (g_copyBack && !g_outW) {
            g_ctx11->CopyResource(g_shared[e].tex11, t);
            if (!g_copyQ) { D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 }; g_dev11->CreateQuery(&qd, &g_copyQ); }
            if (g_copyQ) { g_ctx11->End(g_copyQ); g_copyPending = true; }
        }
        g_arcRect[e] = out;
        return BlitStatic(g_eye[e], t, e, &g_arcRect[e]);
    }
    if (g_outW && (g_outW != g_eyeW || g_outH != g_eyeH)) {
        // upscaling without a DLSS result (DLSS off, starting or failed): the smaller eye image goes in the swapchain's
        // corner and the compositor scales it up through the sub-image rect
        const double sx = (double)g_eyeW / g_outW, sy = (double)g_eyeH / g_outH;
        int32_t x0 = (int32_t)(rect.offset.x * sx + 0.5), y0 = (int32_t)(rect.offset.y * sy + 0.5);
        int32_t x1 = (int32_t)((rect.offset.x + rect.extent.width) * sx + 0.5), y1 = (int32_t)((rect.offset.y + rect.extent.height) * sy + 0.5);
        if (x1 > (int32_t)g_eyeW) x1 = (int32_t)g_eyeW; if (y1 > (int32_t)g_eyeH) y1 = (int32_t)g_eyeH;
        out = { { x0, y0 }, { x1 - x0, y1 - y0 } };
        g_arcRect[e] = out;
        Chain& c = g_eye[e]; uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (!Check(p_xrAcquireSwapchainImage(c.sc, &ai, &idx), "xrAcquireSwapchainImage")) return false;
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
        bool ok = Check(p_xrWaitSwapchainImage(c.sc, &wi), "xrWaitSwapchainImage");
        if (ok) { g_ctx11->CopySubresourceRegion(c.images[idx].texture, 0, 0, 0, 0, g_shared[e].tex11, 0, nullptr); DrawArc(c.images[idx].texture, e, g_arcRect[e]); }
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        p_xrReleaseSwapchainImage(c.sc, &ri);
        return ok;
    }
    g_arcRect[e] = out;
    return BlitToChain(g_eye[e], g_shared[e], e, &g_arcRect[e]);
}
static XrRect2Di g_eyeRect[2]; static bool g_eyeRectSet[2];
void SetEyeRect(int e, int x, int y, int w, int h) { if (e < 0 || e > 1 || w <= 0 || h <= 0) return; g_eyeRect[e] = { { x, y }, { w, h } }; g_eyeRectSet[e] = true; }

// v232: the lowered top strip of the HUD was cut at a fixed row, and a tutorial message crossing that row was split
// in two (its top half lowered, the rest left in place). The HUD's alpha is read back small (a mip level ~256 rows,
// two frames late) and the strip is cut at the empty row nearest to HudTopBand; no empty row nearby = no strip.
static ID3D10Texture2D* g_bandMip = nullptr; static ID3D10ShaderResourceView* g_bandSrv = nullptr; static ID3D10Texture2D* g_bandSt[3] = {};
static int g_bandLevel = 0, g_bandIdx = 0, g_bandFrames = 0; static int32_t g_bandCut = -1; static bool g_bandBad = false;
static void HudBandScan() {
    if (g_bandBad || !g_shared[3].tex10 || !g_dev10) return;
    if (!g_bandMip) {
        g_bandBad = true;
        D3D10_TEXTURE2D_DESC d{}; d.Width = g_eyeW; d.Height = g_eyeH; d.MipLevels = 0; d.ArraySize = 1; d.Format = g_eyeFormat; d.SampleDesc.Count = 1;
        d.Usage = D3D10_USAGE_DEFAULT; d.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE; d.MiscFlags = D3D10_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(g_dev10->CreateTexture2D(&d, nullptr, &g_bandMip)) || FAILED(g_dev10->CreateShaderResourceView(g_bandMip, nullptr, &g_bandSrv))) { LOG("HUD strip: mip texture failed"); return; }
        g_bandLevel = 0; while ((g_eyeH >> (g_bandLevel + 1)) >= 256) ++g_bandLevel;
        D3D10_TEXTURE2D_DESC s{}; s.Width = g_eyeW >> g_bandLevel; s.Height = g_eyeH >> g_bandLevel; if (!s.Width) s.Width = 1; s.MipLevels = 1; s.ArraySize = 1; s.Format = g_eyeFormat; s.SampleDesc.Count = 1;
        s.Usage = D3D10_USAGE_STAGING; s.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        for (auto& t : g_bandSt) if (FAILED(g_dev10->CreateTexture2D(&s, nullptr, &t))) { LOG("HUD strip: staging failed"); return; }
        g_bandBad = false; LOG("HUD strip: cut row from the HUD alpha (mip %d, %ux%u)", g_bandLevel, s.Width, s.Height);
    }
    g_dev10->CopySubresourceRegion(g_bandMip, 0, 0, 0, 0, g_shared[3].tex10, 0, nullptr);
    g_dev10->GenerateMips(g_bandSrv);
    int i = g_bandIdx; g_bandIdx = (g_bandIdx + 1) % 3;
    g_dev10->CopySubresourceRegion(g_bandSt[i], 0, 0, 0, 0, g_bandMip, g_bandLevel, nullptr);
    if (++g_bandFrames < 3) return;
    ID3D10Texture2D* rd = g_bandSt[g_bandIdx];   // written two frames ago
    D3D10_MAPPED_TEXTURE2D m;
    if (FAILED(rd->Map(0, D3D10_MAP_READ, D3D10_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    D3D10_TEXTURE2D_DESC sd; rd->GetDesc(&sd);
    auto empty = [&](int y) { const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch; for (UINT k = 0; k < sd.Width; ++k) if (row[k * 4 + 3] > 2) return false; return true; };
    int want = (int)(sd.Height * g_hudBand), range = (int)(sd.Height * 0.08f) + 1, found = -1;
    for (int d = 0; d <= range && found < 0; ++d) {
        if (want + d < (int)sd.Height && empty(want + d)) found = want + d;
        else if (d && want - d > 0 && empty(want - d)) found = want - d;
    }
    rd->Unmap(0);
    int32_t cut = found < 0 ? -1 : (int32_t)(((found << g_bandLevel)) + (1 << g_bandLevel) / 2);
    if ((cut < 0) != (g_bandCut < 0)) LOG("HUD strip: %s", cut < 0 ? "something crosses the strip line - strip off" : "strip on");
    g_bandCut = cut;
}

void EndFrame(const FrameViews& fv, bool submittedEyes) {
    bool rectSet[2] = { g_eyeRectSet[0], g_eyeRectSet[1] }; g_eyeRectSet[0] = g_eyeRectSet[1] = false;
    if (!g_frameBegun) return;
    XrCompositionLayerProjectionView pv[2]{};
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad hq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad rq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad dq{ XR_TYPE_COMPOSITION_LAYER_QUAD }, mq{ XR_TYPE_COMPOSITION_LAYER_QUAD }, bq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[6] = { (XrCompositionLayerBaseHeader*)&layer, nullptr, nullptr, nullptr, nullptr, nullptr };
    uint32_t layerCount = 0;
    if (submittedEyes && fv.valid && fv.shouldRender) {
        if (g_hudOn && g_hudBandOn && g_hudDrop > 0) HudBandScan();
        Sync10();
        XrRect2Di used[2];
        for (int e = 0; e < 2; ++e) g_arcViews[e] = fv.views[e];
        bool ok0 = BlitEye(0, rectSet[0] ? g_eyeRect[0] : XrRect2Di{ { 0, 0 }, { g_eye[0].w, g_eye[0].h } }, used[0]);
        bool ok1 = BlitEye(1, rectSet[1] ? g_eyeRect[1] : XrRect2Di{ { 0, 0 }, { g_eye[1].w, g_eye[1].h } }, used[1]);
        bool ok = ok0 && ok1;
        if (ok) {
            for (int e = 0; e < 2; ++e) {
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[e].pose = fv.views[e].pose; pv[e].fov = fv.views[e].fov;
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = used[e];
            }
            layer.space = g_local; layer.viewCount = 2; layer.views = pv;
            layerCount = 1;
            if (g_hudOn && BlitToChain(g_hud, g_shared[3])) {
                hq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hq.space = g_hudView ? g_view : g_local; hq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hq.subImage.swapchain = g_hud.sc; hq.subImage.imageRect.extent = { g_hud.w, g_hud.h };
                hq.pose = g_hudPose;
                hq.size = { g_hudWidth, g_hudWidth * (float)g_hud.h / (float)g_hud.w };
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&hq;
                int32_t b = g_bandBad ? (int32_t)(g_hud.h * g_hudBand) : g_bandCut;
                if (g_hudBandOn && b > 0 && b < g_hud.h && g_hudDrop > 0) {
                    const float H = hq.size.height, px = H / (float)g_hud.h;
                    auto shifted = [&](float dy) {   // the HUD pose moved by dy along its own up axis
                        XrPosef p = g_hudPose; const XrQuaternionf& q = p.orientation;
                        float ux = 2 * (q.x * q.y - q.w * q.z), uy = 1 - 2 * (q.x * q.x + q.z * q.z), uz = 2 * (q.y * q.z + q.w * q.x);
                        p.position.x += ux * dy; p.position.y += uy * dy; p.position.z += uz * dy; return p; };
                    bq = hq;   // the top strip, lowered
                    bq.subImage.imageRect.offset = { 0, 0 }; bq.subImage.imageRect.extent = { g_hud.w, b };
                    bq.size = { hq.size.width, b * px };
                    bq.pose = shifted((g_hud.h - b) * 0.5f * px - g_hudDrop * H);
                    hq.subImage.imageRect.offset = { 0, b }; hq.subImage.imageRect.extent = { g_hud.w, g_hud.h - b };   // the rest, where it was
                    hq.size = { hq.size.width, (g_hud.h - b) * px };
                    hq.pose = shifted(-b * 0.5f * px);
                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&bq;
                }
            }
            if (g_retOn && BlitToChain(g_ret, g_shared[4])) {
                XrCompositionLayerQuad& q = rq;
                q = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = g_local; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                int32_t cw = (int32_t)(g_ret.w * g_retFrac), ch = (int32_t)(g_ret.h * g_retFrac);
                q.subImage.swapchain = g_ret.sc;
                q.subImage.imageRect.offset = { (g_ret.w - cw) / 2, (g_ret.h - ch) / 2 };
                q.subImage.imageRect.extent = { cw, ch };
                q.pose = g_retPose;
                q.size = { g_retWidth, g_retWidth * (float)ch / (float)cw };
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&q;
            }
            if (g_dotOn && g_dot.sc && g_dotTex && BlitStatic(g_dot, g_dotTex)) {
                dq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                dq.space = g_local; dq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                dq.subImage.swapchain = g_dot.sc; dq.subImage.imageRect.extent = { g_dot.w, g_dot.h };
                dq.pose = g_dotPose; dq.size = { g_dotSize, g_dotSize };
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&dq;
            }
            if (g_menuOn && g_menu.sc && g_menuTex) {
                if (g_menuDirty) { g_ctx11->UpdateSubresource(g_menuTex, 0, nullptr, g_menuPx, MenuW * 4, 0); g_menuDirty = false; }
                if (BlitStatic(g_menu, g_menuTex)) {
                    mq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    mq.space = g_local; mq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    mq.subImage.swapchain = g_menu.sc; mq.subImage.imageRect.extent = { g_menu.w, g_menu.h };
                    mq.pose = g_menuPose; mq.size = { g_menuWidth, g_menuWidth * (float)MenuH / (float)MenuW };
                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&mq;
                }
            }
        }
    }
    g_hudOn = false; g_retOn = false; g_dotOn = false; g_menuOn = false; g_arcFrame = false;
    g_ctx11->Flush();
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fv.displayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount; ei.layers = layers;
    Check(p_xrEndFrame(g_sess, &ei), "xrEndFrame");
    g_frameBegun = false;
}

// Ends the frame begun by BeginFrame with only a flat quad showing src (scope / binocular view); everything else black.
void EndFrameScreen(const FrameViews& fv, ID3D10Texture2D* src, const XrPosef& pose, float width) {
    if (!g_frameBegun) return;
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    uint32_t layerCount = 0;
    if (src && fv.shouldRender) {
        CopyInto(g_shared[2], src, g_bbMsaa, g_bbFormat);
        Sync10();
        if (BlitToChain(g_screen, g_shared[2])) {
            quad.space = g_local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_screen.sc; quad.subImage.imageRect.extent = { g_screen.w, g_screen.h };
            quad.pose = pose;
            quad.size = { width, width * (float)g_screen.h / (float)g_screen.w };
            layerCount = 1;
        }
    }
    g_hudOn = false; g_retOn = false;
    g_ctx11->Flush();
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fv.displayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount; ei.layers = layers;
    Check(p_xrEndFrame(g_sess, &ei), "xrEndFrame(scope)");
    g_frameBegun = false;
}

// Scope view: square crop of the flat frame + round window/crosshair + a crop of the HUD layer (ammo) beside it.
void EndFrameScope(const FrameViews& fv, ID3D10Texture2D* src, const XrPosef& pose, float width, const float reticleRect[4], const float ammoRect[4], const XrPosef& ammoPose, float ammoWidth, bool srcResolved, bool ownAmmo, bool plain) {
    if (plain) { ammoWidth = 0; ownAmmo = false; }   // v214 monocular: no crosshair, no ammo counter
    if (!g_frameBegun) return;
    XrCompositionLayerQuad q[4]; for (auto& x : q) x = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[4]; uint32_t n = 0;
    auto hudQuad = [&](const float* r, const XrPosef& p, float w) {
        int32_t x0 = (int32_t)r[0], y0 = (int32_t)r[1], rw = (int32_t)r[2], rh = (int32_t)r[3];
        if (rw <= 0 || rh <= 0 || x0 < 0 || y0 < 0 || x0 + rw > g_hud.w || y0 + rh > g_hud.h) return;
        XrCompositionLayerQuad& u = q[n];
        u.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        u.space = g_local; u.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        u.subImage.swapchain = g_hud.sc; u.subImage.imageRect.offset = { x0, y0 }; u.subImage.imageRect.extent = { rw, rh };
        u.pose = p; u.size = { w, w * (float)rh / (float)rw };
        layers[n] = (XrCompositionLayerBaseHeader*)&q[n]; ++n;
    };
    if (src && fv.shouldRender) {
        CopyInto(g_shared[2], src, g_bbMsaa && !srcResolved, g_bbFormat);
        Sync10();
        if (BlitToChain(g_screen, g_shared[2])) {
            int32_t side = g_screen.w < g_screen.h ? g_screen.w : g_screen.h;
            if (plain) side = (int32_t)(side / g_monoScale) & ~1;   // v216: the monocular's lens fills the window
            XrCompositionLayerQuad& a = q[n];
            a.space = g_local; a.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            a.subImage.swapchain = g_screen.sc;
            a.subImage.imageRect.offset = { (g_screen.w - side) / 2, (g_screen.h - side) / 2 };
            a.subImage.imageRect.extent = { side, side };
            a.pose = pose; a.size = { width, width };
            layers[n] = (XrCompositionLayerBaseHeader*)&q[n]; ++n;
            bool hud = g_hud.sc && g_shared[3].tex11 && BlitToChain(g_hud, g_shared[3]);
            if (hud && reticleRect) hudQuad(reticleRect, pose, width);   // the game's own scope reticle
            ID3D11Texture2D* vt = plain ? (g_vigMonoTex[g_monoTarget ? 1 : 0] ? g_vigMonoTex[g_monoTarget ? 1 : 0] : g_vigPlainTex) : g_vigTex; if (!vt) vt = g_vigTex;
            if (g_vig.sc && vt && BlitStatic(g_vig, vt)) {
                XrCompositionLayerQuad& v = q[n];
                v.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                v.space = g_local; v.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                v.subImage.swapchain = g_vig.sc; v.subImage.imageRect.extent = { g_vig.w, g_vig.h };
                v.pose = pose; v.size = { width * 1.002f, width * 1.002f };
                layers[n] = (XrCompositionLayerBaseHeader*)&q[n]; ++n;
            }
            if (ownAmmo && ammoWidth > 0 && g_ammoHave && g_ammo.sc && g_ammoTex) {   // the mod's own counter
                if (g_ammoDirty) { g_ctx11->UpdateSubresource(g_ammoTex, 0, nullptr, g_ammoPx, AmmoW * 4, 0); g_ammoDirty = false; }
                if (BlitStatic(g_ammo, g_ammoTex)) {
                    XrCompositionLayerQuad& u = q[n];
                    u.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    u.space = g_local; u.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    u.subImage.swapchain = g_ammo.sc; u.subImage.imageRect.extent = { g_ammo.w, g_ammo.h };
                    u.pose = ammoPose; u.size = { ammoWidth, ammoWidth * (float)AmmoH / (float)AmmoW };
                    layers[n] = (XrCompositionLayerBaseHeader*)&q[n]; ++n;
                }
            } else if (ammoRect && ammoWidth > 0) {   // the game's ammo counter, cropped from the flat frame (lower right)
                int32_t x0 = (int32_t)ammoRect[0], y0 = (int32_t)ammoRect[1], rw = (int32_t)ammoRect[2], rh = (int32_t)ammoRect[3];
                if (rw > 0 && rh > 0 && x0 >= 0 && y0 >= 0 && x0 + rw <= g_screen.w && y0 + rh <= g_screen.h) {
                    XrCompositionLayerQuad& u = q[n];
                    u.space = g_local; u.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    u.subImage.swapchain = g_screen.sc; u.subImage.imageRect.offset = { x0, y0 }; u.subImage.imageRect.extent = { rw, rh };
                    u.pose = ammoPose; u.size = { ammoWidth, ammoWidth * (float)rh / (float)rw };
                    layers[n] = (XrCompositionLayerBaseHeader*)&q[n]; ++n;
                }
            }
        }
    }
    g_hudOn = false; g_retOn = false; g_dotOn = false;
    g_ctx11->Flush();
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fv.displayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = n; ei.layers = layers;
    Check(p_xrEndFrame(g_sess, &ei), "xrEndFrame(scope)");
    g_frameBegun = false;
}

void SubmitScreen(ID3D10Texture2D* src, const XrPosef* pose, float width) {
    if (!SessionRunning() || g_frameBegun) return;
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (!WaitBegin(fs)) return;
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    uint32_t layerCount = 0;
    if (fs.shouldRender) {
        CopyInto(g_shared[2], src, g_bbMsaa, g_bbFormat);
        Sync10();
        if (BlitToChain(g_screen, g_shared[2])) {
            quad.space = g_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_screen.sc;
            quad.subImage.imageRect.extent = { g_screen.w, g_screen.h };
            if (pose) quad.pose = *pose;
            else { quad.pose.orientation.w = 1.0f; quad.pose.position = { 0.0f, 0.0f, -2.0f }; }
            quad.size = { width, width * (float)g_screen.h / (float)g_screen.w };
            layerCount = 1;
        }
    }
    g_ctx11->Flush();
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount; ei.layers = layers;
    Check(p_xrEndFrame(g_sess, &ei), "xrEndFrame(screen)");
    g_frameBegun = false;
}

void Shutdown() {
    if (g_sess && g_running) { p_xrRequestExitSession(g_sess); }
    // Process is exiting; the runtime and driver reclaim resources. Avoid teardown under loader lock.
}

} // namespace xr
