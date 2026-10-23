// controls.cpp - Quest/Touch motion controllers -> the game's own Xbox gamepad input.
// Dunia reads the pad through XInputGetState (xinput1_3 ordinal 2) and rumbles it through XInputSetState
// (ordinal 3). Both imports are redirected here: while VR controllers are active, pad 0 is synthesised from them,
// so the game's gamepad bindings, hints and menus all work unchanged.
#include "controls.h"
#include "xr.h"
#include "log.h"
#include "gameaddr.h"
#include <xinput.h>
#include <cmath>
#include <cstring>

namespace controls {
static volatile float g_rtValue = 0;
static volatile bool g_mono = false, g_monoFace = false;   // v212 monocular: out (the aim button held for the game), at the face (scope view, trigger tags)
bool MonocularOut() { return g_mono; }
bool MonocularAtFace() { return g_mono && g_monoFace; }
static volatile bool g_flash = false;
static volatile LONG g_recenterReq = 0;   // v222
bool TakeRecenterRequest() { return InterlockedExchange(&g_recenterReq, 0) != 0; }   // v220 flashlight switched on
bool FlashlightOn() { return g_flash; }
float RightTriggerValue() { return g_rtValue; }

static Config g_c;
typedef DWORD (WINAPI *XGetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *XSetState_t)(DWORD, XINPUT_VIBRATION*);
static XGetState_t o_GetState = nullptr;
static XSetState_t o_SetState = nullptr;
static volatile LONG g_active = 0;     // VR controllers are driving pad 0

// ---------------------------------------------------------------- IAT patch
static bool PatchIat(HMODULE mod, const char* dll, WORD ordinal, void* hook, void** orig) {
    uint8_t* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base; auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) || IMAGE_ORDINAL(names->u1.Ordinal) != ordinal) continue;
            DWORD old; VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (!*orig) *orig = (void*)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            done = true;
        }
    }
    return done;
}

// ---------------------------------------------------------------- small math (OpenXR LOCAL: +Y up, -Z forward)
struct V { float x, y, z; };
static V Rot(const XrQuaternionf& q, V v) {
    V u{ q.x, q.y, q.z }; float s = q.w;
    float d = u.x * v.x + u.y * v.y + u.z * v.z, uu = u.x * u.x + u.y * u.y + u.z * u.z;
    V c{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    return { 2 * d * u.x + (s * s - uu) * v.x + 2 * s * c.x, 2 * d * u.y + (s * s - uu) * v.y + 2 * s * c.y, 2 * d * u.z + (s * s - uu) * v.z + 2 * s * c.z };
}

// ---------------------------------------------------------------- mapping state (game thread)
enum Zone { Z_NONE, Z_SHOULDER, Z_HIP, Z_CHEST };
static const char* ZoneName(int z) { static const char* n[] = { "none", "shoulder", "hip", "chest" }; return n[z]; }
struct Pulse { WORD btn = 0; ULONGLONG until = 0; };
static Pulse g_pulse[4];
static void PulseBtn(WORD b, ULONGLONG now, int ms = 90) { for (auto& p : g_pulse) if (p.until <= now) { p.btn = b; p.until = now + ms; return; } }

static bool g_crouchDown = false, g_sightOn = false; static volatile ULONGLONG g_usePulse = 0;
static bool g_lGripDown = false, g_lGripNoUse = false; static ULONGLONG g_lGripTime = 0; static volatile bool g_leftClaimed = false;
static bool g_rGripDown = false, g_rGripAim = false;        // right grip held; started outside a holster zone (= iron sight)
static bool g_lTrigDown = false, g_lTrigGrenade = false;
static volatile int g_aimHand = -1; static volatile LONG g_throwRelease = 0, g_throwFire = 0;
static bool g_bDown = false, g_bHoldFired = false; static ULONGLONG g_bTime = 0;
static int g_shoulderNext = 0;                               // 0 = primary next, 1 = special next
static volatile bool g_sightsAllowed = true;
static int g_cycle = 0; static volatile bool g_mapOpen = false; static volatile bool g_zoneNow = false;
static const WORD kSlots[4] = { XINPUT_GAMEPAD_DPAD_LEFT /*primary*/, XINPUT_GAMEPAD_DPAD_RIGHT /*secondary*/,
                                XINPUT_GAMEPAD_DPAD_DOWN /*special*/, XINPUT_GAMEPAD_DPAD_UP /*machete*/ };
static XINPUT_GAMEPAD g_last{}; static DWORD g_packet = 1;
static int g_lastZoneLogged = -1;

static Zone RightHandZone(const xr::HandState& r, const XrPosef& head) {
    if (!r.gripValid) return Z_NONE;
    V hf = Rot(head.orientation, { 0, 0, -1 });
    float hl = sqrtf(hf.x * hf.x + hf.z * hf.z); if (hl < 1e-4f) return Z_NONE;
    V fwd{ hf.x / hl, 0, hf.z / hl }, right{ -fwd.z, 0, fwd.x };
    V d{ r.grip.position.x - head.position.x, r.grip.position.y - head.position.y, r.grip.position.z - head.position.z };
    float f = d.x * fwd.x + d.z * fwd.z, rt = d.x * right.x + d.z * right.z, up = d.y;
    V pointing = Rot(r.grip.orientation, { 0, 0, -1 });
    if (f < g_c.shoulderFwd && up > g_c.shoulderUp && rt > 0.0f) return Z_SHOULDER;          // reach over the right shoulder
    if (up < g_c.hipUp && pointing.y < -0.55f) return Z_HIP;                                // hand low, pointing down
    if (up < g_c.chestUpMax && up > g_c.chestUpMin && f > -0.05f && f < 0.35f && fabsf(rt) < 0.2f) return Z_CHEST;
    return Z_NONE;
}

static inline SHORT Axis(float v) { v = v > 1 ? 1 : v < -1 ? -1 : v; return (SHORT)lroundf(v * 32767.0f); }

// ---------------------------------------------------------------- vehicles: the game's active action map
// The player's input object keeps a stack of action maps (Config\InputActionMap*.xml); the top one says what the
// controls do now: "driving" (driver seat), "passenger", "using_mounted_weapon" (turrets), ... Its name id (the
// engine's CRC32 of the name, 0x10229400) sits at +0x68 of the map. Path, as the game's SetPlayerActionMap
// (0x1058F930): [0x11645578] -> +4 -> [0] -> +4 -> +0xE4 = stack; 0x104F0720(stack, -1) = top map.
static uintptr_t g_base = 0;
static inline uintptr_t GA(uintptr_t va) { uint32_t v = gameaddr::Xlate((uint32_t)va); return v ? g_base + (v - 0x10000000) : 0; }
struct MapName { const char* name; int mode; uint32_t id; };
static MapName g_maps[] = {
    { "driving", 1, 0 }, { "map_driving", 1, 0 },
    { "passenger", 2, 0 }, { "in_vehicle", 2, 0 }, { "ied_in_vehicle", 2, 0 }, { "vehicle_transitions", 2, 0 },
    { "using_mounted_weapon", 2, 0 }, { "paragliderdriving", 2, 0 },
    { "gameplay", 0, 0 }, { "default", 0, 0 }, { "ladder", 0, 0 }, { "watch", 0, 0 }, { "menus", 0, 0 }, { "pausemenu", 0, 0 },
    { "cinematic", 0, 0 }, { "mapcompass", 0, 0 }, { "lyingdown", 0, 0 }, { "machete_finish", 0, 0 }, { "healPlayer", 0, 0 },
};
static volatile int g_vehMode = 0; static volatile bool g_mounted = false; static volatile bool g_normalMap = true; static volatile bool g_menuMap = false; static volatile int g_mapCompass = -1; static volatile bool g_itemMap = false; static volatile bool g_loadingMap = false;   // -1 unknown, else the game's map action map is (1) / is not (0) up
static uint32_t TopActionMap() {
    if (!g_base) return 0;
    auto ok = [](const void* p, size_t n) { return p && !IsBadReadPtr(p, n); };
    uint8_t** pD = (uint8_t**)GA(0x11645578); if (!ok(pD, 4)) return 0;
    uint8_t* D = *pD; if (!ok(D, 12) || *(int*)(D + 8) == 0) return 0;
    uint8_t** arr = *(uint8_t***)(D + 4); if (!ok(arr, 4)) return 0;
    uint8_t* p0 = arr[0]; if (!ok(p0, 8)) return 0;
    uint8_t* inner = *(uint8_t**)(p0 + 4); if (!ok(inner, 0x120)) return 0;
    uint8_t* st = inner + 0xE4;
    if (*(uint32_t*)(st + 0x30) == 0 || *(uint32_t*)(st + 0x30) > 64 || !ok(*(void**)(st + 0x2C), *(uint32_t*)(st + 0x30) * 16)) return 0;
    typedef uint8_t* (__fastcall *Top_t)(void* stack, void* edx, int prio);
    uint8_t* m = ((Top_t)GA(0x104F0720))(st, nullptr, -1);
    return ok(m, 0x6C) ? *(uint32_t*)(m + 0x68) : 0;
}
static void UpdateVehicleMode() {
    static bool ids = false;
    if (!ids && g_base) {
        typedef uint32_t (__cdecl *Crc_t)(const char*);
        for (auto& m : g_maps) m.id = ((Crc_t)GA(0x10229400))(m.name);
        ids = true;
    }
    uint32_t id = TopActionMap();
    static uint32_t last = 1;
    int mode = 0; const char* nm = nullptr;
    for (auto& m : g_maps) if (m.id == id) { mode = m.mode; nm = m.name; break; }
    g_loadingMap = id == 0 || id == 0x405707AEu;
    if (id != last) { last = id; if (nm) LOG("controls: action map %s (%s)", nm, mode == 1 ? "driving" : mode == 2 ? "vehicle seat / mounted" : "on foot"); else LOG("controls: action map %08x", id); }
    if (mode != g_vehMode) { g_vehMode = mode; }
    g_normalMap = id == 0 || (nm && (!strcmp(nm, "default") || !strcmp(nm, "gameplay")));
    g_menuMap = nm && (!strcmp(nm, "pausemenu") || !strcmp(nm, "menus"));
    g_mounted = nm && !strcmp(nm, "using_mounted_weapon");
    g_itemMap = nm && (!strcmp(nm, "watch") || !strcmp(nm, "mapcompass") || !strcmp(nm, "map_driving") || !strcmp(nm, "pausemenu") || !strcmp(nm, "menus") || !strcmp(nm, "cinematic") || !strcmp(nm, "healPlayer"));
    g_mapCompass = id == 0 ? -1 : (nm && (!strcmp(nm, "mapcompass") || !strcmp(nm, "map_driving"))) ? 1 : 0;
}
int VehicleMode() { return g_vehMode; }
int ThrowAimHand() { return g_aimHand; }
int TakeThrowRelease() { return (int)InterlockedExchange(&g_throwRelease, 0); }
void FireThrow() { InterlockedExchange(&g_throwFire, 1); }
bool NormalControls() { return g_normalMap; }
bool MapControls() { return g_mapCompass == 1; }
bool LoadingControls() { return g_loadingMap; }
bool MenuControls() { return g_menuMap; }
bool MountedWeapon() { return g_mounted; }

static volatile LONG g_holsterReq = 0, g_selectReq = 0;   // left grip + B: put the weapon away / bring it back; a weapon was selected
static volatile bool g_mapReal = false;   // v219: the map controls are up for the map itself (not a phone call)
static volatile bool g_phoneOut = false; static ULONGLONG g_phoneT = 0;
bool PhoneOut() { return g_phoneOut; }
bool ItemControls() { return g_itemMap || g_phoneOut; }
void ClearPhone() { if (g_phoneOut) { g_phoneOut = false; LOG("controls: phone away (weapon back)"); } }
static volatile LONG g_recapReq = 0;
int TakeRecapture() { return (int)InterlockedExchange(&g_recapReq, 0); }
int TakeHolsterToggle() { return (int)InterlockedExchange(&g_holsterReq, 0); }
int TakeWeaponSelect() { return (int)InterlockedExchange(&g_selectReq, 0); }
// the weapon is put away (left grip + B): the game's holster lock also keeps the map / GPS (diamond tracker) and the
// phone from coming out, so a press of that button first asks arms to lift the lock, and reaches the game a moment later
static volatile LONG g_reloadPress = 0;   // the reload button (left trigger) was pressed
int TakeReloadPress() { return (int)InterlockedExchange(&g_reloadPress, 0); }
static volatile bool g_rightFree = false;   // no weapon in the right hand (arms)
void SetRightHandFree(bool f) { g_rightFree = f; }
static volatile bool g_handsAway = false; static volatile LONG g_mapLiftReq = 0;
void SetHandsAway(bool a) { g_handsAway = a; }
int TakeMapLift() { return (int)InterlockedExchange(&g_mapLiftReq, 0); }
static volatile bool g_menuMode = false;
static volatile LONG g_snapReq = 0; static volatile float g_snapStick = 0; static volatile DWORD g_watchTick = 0;
int TakeSnap() { return (int)InterlockedExchange(&g_snapReq, 0); }
void SetSnapStick(float x) { g_snapStick = x; }
float SnapDegrees() { return g_c.snapTurn; }
// v224 smooth turning speed (Home menu): 1.0 = the game's full-stick rate (~150 deg/s). Below: a smaller stick push
// (the game's dead zone ends near 0.4); above: full stick plus the mod's own extra turn (arms, as the snap turn does)
static volatile float g_turnSpeed = 1.0f; static volatile float g_turnExtra = 0.0f;
void SetTurnSpeed(float k) { g_turnSpeed = k < 0.2f ? 0.2f : k > 3.0f ? 3.0f : k; }
float TurnSpeed() { return g_turnSpeed; }
float TurnExtra() { return g_turnExtra; }
void SetWatchActive() { g_watchTick = GetTickCount(); }
bool WatchActive() { return GetTickCount() - g_watchTick < 250; }
void SetMenuMode(bool on) { g_menuMode = on; }
struct MappingHistory {
    ULONGLONG recenterAt = 0, gadgetAt = 0, mapPress = 0, notMapSince = 0;
    bool recenterFired = false, driverMap = false, gadgetHeld = false, gadgetDelayed = false, mapPrev = false;
    int lastMap = -1, turnDir = 0, snapDir = 0;
    bool aPrev = false, aCombo = false, rThrow = false, lThrow = false, trigFlash = false, bCombo = false;
};
static MappingHistory g_history;
static fc2vr::authority::SampleCursor g_inputCursor;
static uint64_t g_mappingGeneration = 0;
void CancelPendingInput() {
    g_history = {}; g_inputCursor.Clear(); g_mappingGeneration = 0;
    g_crouchDown = g_sightOn = g_lGripDown = g_lGripNoUse = g_rGripDown = g_rGripAim = false;
    g_lTrigDown = g_lTrigGrenade = g_bDown = g_bHoldFired = false;
    g_lGripTime = g_bTime = 0; g_rtValue = g_turnExtra = 0; g_snapStick = 0;
    g_aimHand = -1; g_monoFace = g_zoneNow = g_leftClaimed = false; g_usePulse = 0;
    for (auto& p : g_pulse) p = {};
    InterlockedExchange(&g_throwRelease,0); InterlockedExchange(&g_throwFire,0);
    InterlockedExchange(&g_snapReq,0); InterlockedExchange(&g_reloadPress,0);
    InterlockedExchange(&g_holsterReq,0); InterlockedExchange(&g_selectReq,0);
    InterlockedExchange(&g_recapReq,0); InterlockedExchange(&g_mapLiftReq,0); InterlockedExchange(&g_recenterReq,0);
}
static void Build(XINPUT_GAMEPAD& g, const xr::HandState* h, const XrPosef& head, bool headValid) {
    memset(&g, 0, sizeof g);
    xr::HandState hm[2];
    if (g_menuMode) { hm[0] = h[0]; hm[1] = h[1]; for (auto& x : hm) { x.stick.x = x.stick.y = 0; x.stickClick = false; } h = hm; }   // the sticks drive the mod's menu
    ULONGLONG now = GetTickCount64();
    const xr::HandState& L = h[0]; const xr::HandState& R = h[1];
    // v222 recenter: the left stick held in for RecenterHoldMs (the click still sprints as usual)
    { auto& downAt = g_history.recenterAt; auto& fired = g_history.recenterFired;
      if (L.stickClick) { if (!downAt) { downAt = now; fired = false; }
          else if (!fired && g_c.recenterHoldMs > 0 && now - downAt >= (ULONGLONG)g_c.recenterHoldMs) { fired = true; InterlockedExchange(&g_recenterReq, 1); xr::Haptic(0, 0.5f, 0.08f); LOG("controls: left stick held - recenter"); } }
      else downAt = 0; }
    WORD b = 0;
    UpdateVehicleMode();
    const bool poseFire = g_vehMode != 0 || g_mounted || g_menuMap || (R.gripValid && R.aimValid);
    g_rtValue = poseFire ? R.trigger : 0;
    if (g_vehMode == 1 && !g_menuMode) {   // driver seat (VEHICLE_CONTROLS): the game's pad driving map, Config\InputActionMap.xml
        // steering = right stick left/right (or the left stick), RT gas, LT brake/reverse, left grip handbrake (pad B),
        // right grip change seat (pad A), X exit (pad Y), right stick click heal (pad LB), left Y map, menu = pause
        float sx = fabsf(R.stick.x) >= fabsf(L.stick.x) ? R.stick.x : L.stick.x;
        g.sThumbLX = Axis(sx); g.sThumbLY = Axis(L.stick.y);
        g.bRightTrigger = (BYTE)lroundf(fminf(1.0f, R.trigger) * 255.0f);
        g.bLeftTrigger = (BYTE)lroundf(fminf(1.0f, L.trigger) * 255.0f);
        if (L.squeeze > 0.5f) b |= XINPUT_GAMEPAD_B;               // handbrake (held)
        if (R.squeeze > 0.6f) b |= XINPUT_GAMEPAD_A;               // change seat
        if (R.btn1) b |= XINPUT_GAMEPAD_A;
        if (L.btn1) b |= XINPUT_GAMEPAD_Y;                         // exit / bail out
        if (R.stickClick) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;       // heal
        if (L.btn2) b |= XINPUT_GAMEPAD_BACK;                      // map
        { auto& prev = g_history.driverMap; if (L.btn2 && !prev) g_mapOpen = !g_mapOpen; prev = L.btn2; }
        if (L.menu) b |= XINPUT_GAMEPAD_START;
        if (R.btn2) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;            // B: headlights
        g_sightOn = false; g_zoneNow = false; g_rGripDown = R.squeeze > 0.6f; g_lGripDown = L.squeeze > 0.5f;
        for (auto& p : g_pulse) if (p.until > now) b |= p.btn;
        g.wButtons = b;
        return;
    }
    // left hand
    g.sThumbLX = Axis(L.stick.x); g.sThumbLY = Axis(L.stick.y);
    if (L.stickClick) b |= XINPUT_GAMEPAD_LEFT_THUMB;      // sprint
    if (L.btn1 && !g_menuMode) b |= XINPUT_GAMEPAD_Y;     // X: interact / use (E) - doors, turrets, weapons on the ground
    { auto& held = g_history.gadgetHeld; auto& delayed = g_history.gadgetDelayed; auto& at = g_history.gadgetAt;   // map / gadget
      if (L.btn2 && !held) { delayed = g_handsAway; if (delayed) { InterlockedExchange(&g_mapLiftReq, 1); at = now + 150; LOG("controls: map button with the weapon away - lifting the holster lock first"); } }
      held = L.btn2;
      if (!L.btn2) delayed = false;
      if (L.btn2 && !delayed) b |= XINPUT_GAMEPAD_BACK;
      if (at && now >= at) { PulseBtn(XINPUT_GAMEPAD_BACK, now, 150); at = 0; } }
    // the map / GPS is out while the game's map controls are up ("mapcompass"). The same button answers the phone,
    // so counting presses would leave the hands in map mode after a call (the rifle turned by the map angles).
    // The button press still counts until the game's controls catch up (~0.5 s), and as a fallback.
    // The phone: answering a call (same button) shows the game's map controls for ~1.5 s, then the game closes
    // them itself; the map stays up until the button is pressed again. So map controls ending without a press
    // = the phone is out (until the weapon comes back - arms clears it - or the button is pressed again).
    { auto& prev = g_history.mapPrev; auto& pressT = g_history.mapPress; auto& lastMc = g_history.lastMap;
      if (L.btn2 && !prev && g_mono) { g_mono = false; LOG("controls: monocular away (map button)"); }
      if (L.btn2 && !prev) { g_mapOpen = !g_mapOpen; pressT = now; if (g_phoneOut) { g_phoneOut = false; LOG("controls: phone away (button)"); } LOG("controls: map button (%s)", g_mapOpen ? "out" : "away"); }
      prev = L.btn2;
      if (lastMc == 1 && g_mapCompass == 0 && g_normalMap && now - pressT > 600) { g_phoneOut = true; g_phoneT = now; LOG("controls: the game closed its map controls by itself - phone out"); }
      // v219: the map's controls also come up for a phone call (answered with the same button). A map opened with the
      // button shows them within ~0.2 s; a call takes ~2 s (the phone animation). Only the real map offers the monocular.
      if (lastMc != 1 && g_mapCompass == 1) { g_mapReal = now - pressT < 1000; LOG("controls: map controls up %llu ms after the button - %s", now - pressT, g_mapReal ? "the map" : "a phone call (no monocular)"); }
      if (g_mapCompass != 1) g_mapReal = false;
      lastMc = g_mapCompass;
      if (g_phoneOut && (now - g_phoneT > 180000 || g_vehMode != 0)) g_phoneOut = false;
      if (g_mapCompass >= 0 && now - pressT > 1500) {
          bool m = g_mapCompass == 1;
          if (m != g_mapOpen) { g_mapOpen = m; LOG("controls: map %s (game controls)", m ? "out" : "away"); }
      } }
    if (L.menu) b |= XINPUT_GAMEPAD_START;                 // pause menu
    // left grip: use / pick up / enter-exit vehicle, unless it is gripping the weapon (two-handed)
    {
        bool lg = L.squeeze > (g_lGripDown ? 0.35f : 0.6f);
        if (lg && !g_lGripDown) { g_lGripTime = now; g_lGripNoUse = false; }
        g_lGripDown = lg;
        if (lg && g_leftClaimed) g_lGripNoUse = true;          // no 'use' for the rest of this press
    }
    // right hand
    // right stick: left/right turns; pulled down = crouch (pad B; looking up/down is done with the head)
    bool down = !g_mounted && R.stick.y < (g_crouchDown ? -0.45f : -0.7f) && fabsf(R.stick.x) < fabsf(R.stick.y);   // v221: no crouch on a mounted gun
    g_crouchDown = down;
    // turning: any clear left/right push turns at full speed, even on a diagonal (a diagonal only gives ~70% of x)
    {
        auto& turnDir = g_history.turnDir;
        float x = R.stick.x;
        if (turnDir == 0) { if (x > 0.35f) turnDir = 1; else if (x < -0.35f) turnDir = -1; }
        else if (turnDir * x < 0.25f) turnDir = 0;
        g.sThumbRX = down ? 0 : g_c.turnFull ? (SHORT)(turnDir * 32767) : Axis(x);
        {   // v224 turn speed
            float k = g_turnSpeed, mag = k >= 1.0f ? 1.0f : 0.42f + 0.58f * k;
            if (k != 1.0f && g.sThumbRX != 0) g.sThumbRX = (SHORT)lroundf((float)g.sThumbRX * mag);
            float push = g_c.turnFull ? (float)turnDir : (fabsf(x) > 0.25f ? x : 0.0f);
            g_turnExtra = (k > 1.0f && !down && g_c.snapTurn < 0.5f && g_vehMode == 0 && !g_mounted) ? push * (k - 1.0f) : 0.0f;
        }
        bool watch = GetTickCount() - g_watchTick < 250;   // the watch is up (safehouse sleep): the stick sets the time instead
        if (watch) g.sThumbRX = 0;
        if (g_c.snapTurn > 0.5f && g_vehMode == 0) {   // snap turning: one push = one turn of SnapTurn degrees (done by the mod, see arms)
            auto& snapDir = g_history.snapDir;
            if (snapDir == 0) { if (x > 0.6f) snapDir = 1; else if (x < -0.6f) snapDir = -1; if (snapDir && !down && !watch) InterlockedExchangeAdd(&g_snapReq, snapDir); }
            else if (snapDir * x < 0.3f) snapDir = 0;
            float sb = g_snapStick;   // a turn the game has to do itself (fallback): the mod steers the stick
            g.sThumbRX = sb != 0.0f ? Axis(sb) : 0;
        }
    }
    g.sThumbRY = g_c.stickPitch && !down ? Axis(R.stick.y) : 0;
    // v221 mounted guns (turrets): either stick aims - left/right turns the gun, up/down raises and lowers it (the
    // game's pad aiming, right stick); the left stick doesn't walk there
    if (g_mounted) {
        float ax = fabsf(R.stick.x) >= fabsf(L.stick.x) ? R.stick.x : L.stick.x;
        float ay = fabsf(R.stick.y) >= fabsf(L.stick.y) ? R.stick.y : L.stick.y;
        g.sThumbRX = Axis(ax); g.sThumbRY = Axis(g_c.turretInvertY ? -ay : ay);
        g.sThumbLX = g.sThumbLY = 0;
    }
    if (down) b |= XINPUT_GAMEPAD_B;                       // crouch
    // left grip + A: take the weapon's hold again (no jump)
    { auto& aPrev = g_history.aPrev; auto& aCombo = g_history.aCombo;
      if (R.btn1 && !aPrev && L.squeeze > 0.6f) { aCombo = true; InterlockedExchange(&g_recapReq, 1); LOG("controls: left grip + A - take the hold again"); }
      if (!R.btn1) aCombo = false;
      aPrev = R.btn1;
      if (R.btn1 && !aCombo) b |= XINPUT_GAMEPAD_A; }      // jump / change seat
    if (R.stickClick) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;   // right stick click: syrette (heal yourself)
    g.bRightTrigger = poseFire ? (BYTE)lroundf(fminf(1.0f, R.trigger) * 255.0f) : 0;   // pose-guided fire needs tracked input; native vehicle/turret controls do not
    if (!g_c.macheteTrigger && MacheteSelected()) g.bRightTrigger = 0;   // machete: hits come from swinging it; the game's attack animation sways the camera
    // right grip: holster grab (weapon select) in a body zone, iron sight / reverse elsewhere
    Zone z = headValid && g_vehMode == 0 ? RightHandZone(R, head) : Z_NONE;   // no holster grabs in a vehicle seat
    g_zoneNow = z != Z_NONE;
    bool grip = R.squeeze > (g_rGripDown ? 0.35f : 0.6f);
    // v212 monocular: with the map / GPS up (or the monocular out), a right-grip press away from the holster spots takes the
    // monocular out (the game raises it while its aim button is held) or puts it back (the map and GPS return)
    bool monoToggle = grip && !g_rGripDown && z == Z_NONE && g_c.monocular && g_vehMode == 0 && (g_mono || (g_mapCompass == 1 && g_mapReal && !g_phoneOut));
    if (monoToggle) {
        g_mono = !g_mono; g_monoFace = false; g_rGripAim = false; xr::Haptic(1, 0.5f, 0.05f);
        LOG("controls: monocular %s (right grip)", g_mono ? "out" : "away - map and GPS back");
    } else if (grip && !g_rGripDown) {
        if (z != Z_NONE) { g_mapOpen = false; if (g_mono) { g_mono = false; LOG("controls: monocular away (holster grab)"); } }
        if (z == Z_SHOULDER) { PulseBtn(g_shoulderNext ? kSlots[2] : kSlots[0], now); g_cycle = g_shoulderNext ? 2 : 0; g_shoulderNext ^= 1; g_rGripAim = false; xr::Haptic(1, 0.6f, 0.06f); }
        else if (z == Z_HIP) { PulseBtn(kSlots[1], now); g_cycle = 1; g_rGripAim = false; xr::Haptic(1, 0.6f, 0.06f); }
        else if (z == Z_CHEST) { PulseBtn(kSlots[3], now); g_cycle = 3; g_rGripAim = false; xr::Haptic(1, 0.6f, 0.06f); }
        else g_rGripAim = true;
        if (z != Z_NONE) { LOG("controls: holster grab at %s", ZoneName(z)); InterlockedExchange(&g_selectReq, 1); }
    }
    g_rGripDown = grip; if (!grip) g_rGripAim = false;
    if (g_vehMode == 2 && grip) { b |= XINPUT_GAMEPAD_A; g_rGripAim = false; }   // passenger / gunner seat: right grip changes seat
    // iron sight / scope: hold the weapon up to your face (CrysisVR: on within 0.25 m of the head, off beyond 0.35 m)
    if (headValid && R.gripValid) {
        float dx = R.grip.position.x - head.position.x, dy = R.grip.position.y - head.position.y, dz = R.grip.position.z - head.position.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        bool was = g_sightOn;
        g_sightOn = (g_leftClaimed || !g_c.sightNeedsGrip) && d < (g_sightOn ? g_c.sightOff : g_c.sightOn);
        if (g_sightOn != was) { xr::Haptic(1, 0.3f, 0.03f); LOG("controls: sights %s (hand %.2f m from head)", g_sightOn ? "up" : "down", d); }
    } else g_sightOn = false;
    if (g_sightOn && g_sightsAllowed) g.bLeftTrigger = 255;
    if (g_mono) {
        if (headValid && R.gripValid) {
            float dx = R.grip.position.x - head.position.x, dy = R.grip.position.y - head.position.y, dz = R.grip.position.z - head.position.z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz); bool was = g_monoFace;
            g_monoFace = d < (g_monoFace ? g_c.sightOff : g_c.sightOn);
            if (g_monoFace != was) { xr::Haptic(1, 0.3f, 0.03f); LOG("controls: monocular %s (hand %.2f m from head)", g_monoFace ? "at the eye" : "lowered", d); }
        }
        g.bLeftTrigger = 255;                                    // the game keeps the monocular raised
        g.bRightTrigger = g_monoFace ? g.bRightTrigger : 0;     // tagging only while looking through it
        // the game left the map / monocular (a cut scene, a vehicle, the map closed by the game): monocular off
        auto& notMapSince = g_history.notMapSince;
        if (g_mapCompass == 1) notMapSince = 0; else if (!notMapSince) notMapSince = now;
        if (notMapSince && now - notMapSince > 4000 && (g_itemMap || g_vehMode != 0)) { g_mono = false; g_monoFace = false; notMapSince = 0; LOG("controls: monocular off (the game left the map controls)"); }
        // v217: any other controls taking over (a game message box after tagging, a cut scene): monocular off at once so the
        // message shows (the UI is hidden while the monocular is lowered) and its buttons work - the aim button is let go
        else if (notMapSince && now - notMapSince > 300 && g_mapCompass == 0) { g_mono = false; g_monoFace = false; notMapSince = 0; LOG("controls: monocular off (the game switched to other controls)"); }
    } else g_monoFace = false;
    // throwing arc: hold a hand's grip, then pull its trigger (left hand: not while it holds the weapon); the mod draws
    // the arc and lands the throw where it ends; letting go of either throws (see arms)
    {
        auto& rPrev = g_history.rThrow; auto& lPrev = g_history.lThrow;
        bool rT = R.trigger > 0.6f, lT = L.trigger > 0.6f;
        bool allowed = g_c.throwArc && g_vehMode == 0 && g_normalMap && !g_mapOpen;
        bool lostPose = g_aimHand >= 0 && !(h[g_aimHand].gripValid && h[g_aimHand].aimValid);
        if (!allowed || lostPose) { g_aimHand = -1; InterlockedExchange(&g_throwRelease,0); InterlockedExchange(&g_throwFire,0); }
        else if (g_aimHand < 0) {
            // only a free hand aims a throw: the right one with no weapon in it, the left one when it isn't on the weapon
            if (g_c.throwArcRight && g_rightFree && rT && !rPrev && g_rGripAim && R.gripValid && R.aimValid) g_aimHand = 1;
            else if (lT && !lPrev && g_lGripDown && !g_leftClaimed && L.gripValid && L.aimValid) g_aimHand = 0;
            if (g_aimHand >= 0) { xr::Haptic(g_aimHand, 0.35f, 0.05f); LOG("controls: %s grip + trigger - aiming a throw", g_aimHand ? "right" : "left"); }
        } else {
            const xr::HandState& H = g_aimHand ? R : L;
            if (!(H.trigger > 0.35f && H.squeeze > 0.35f)) { InterlockedExchange(&g_throwRelease, 1); g_aimHand = -1; }
        }
        rPrev = rT; lPrev = lT;
        if (g_c.throwArc && g_c.throwArcRight && ((g_rGripAim && g_rightFree) || g_aimHand == 1) && g_vehMode == 0) g.bRightTrigger = 0;   // right grip held: the trigger aims a throw, it doesn't fire
        if (InterlockedExchange(&g_throwFire, 0)) PulseBtn(XINPUT_GAMEPAD_RIGHT_SHOULDER, now, 120);
    }
    // left trigger: reload / unjam, or grenade throw while the right grip (aim) is held
    bool lt = L.trigger > (g_lTrigDown ? 0.35f : 0.6f) && g_aimHand != 0 && !(g_lGripDown && !g_leftClaimed && g_c.throwArc);
    // v220 flashlight: the left hand raised to the left temple + the left trigger switches it (no reload / throw then)
    auto& g_lTrigFlash = g_history.trigFlash;
    if (lt && !g_lTrigDown && g_c.flashlight && headValid && L.gripValid) {
        const XrQuaternionf& q = head.orientation;   // the head's right axis (+X rotated)
        float rx = 1 - 2 * (q.y * q.y + q.z * q.z), ry = 2 * (q.x * q.y + q.w * q.z), rz = 2 * (q.x * q.z - q.w * q.y);
        float tx = head.position.x - rx * 0.08f, ty = head.position.y - ry * 0.08f, tz = head.position.z - rz * 0.08f;   // left temple
        float dx = L.grip.position.x - tx, dy = L.grip.position.y - ty, dz = L.grip.position.z - tz;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < g_c.flashRadius) { g_flash = !g_flash; g_lTrigFlash = true; xr::Haptic(0, 0.5f, 0.06f); LOG("controls: flashlight %s (left hand %.2f m from the temple)", g_flash ? "on" : "off", d); }
    }
    if (!lt) g_lTrigFlash = false;
    if (lt && !g_lTrigDown && !g_lTrigFlash) { g_lTrigGrenade = g_rGripAim; if (!g_lTrigGrenade) InterlockedExchange(&g_reloadPress, 1); }
    g_lTrigDown = lt;
    if (lt && !g_lTrigFlash) b |= g_lTrigGrenade ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_X;
    // B: tap = next weapon slot, hold = next throwable type (grenade / molotov)
    // left grip + B: put the weapon away (or bring it back) - a combo, so a single press in a fight can't do it
    auto& g_bCombo = g_history.bCombo;
    if (R.btn2 && !g_bDown && L.squeeze > 0.6f) { g_bCombo = true; InterlockedExchange(&g_holsterReq, 1); xr::Haptic(1, 0.5f, 0.05f); LOG("controls: left grip + B - weapon away / back"); }
    if (g_bCombo) { if (!R.btn2) g_bCombo = false; g_bDown = R.btn2; g_bHoldFired = true; }
    if (R.btn2 && !g_bDown) { g_bTime = now; g_bHoldFired = false; }
    if (R.btn2 && !g_bHoldFired && now - g_bTime >= (ULONGLONG)g_c.holdMs) { PulseBtn(XINPUT_GAMEPAD_RIGHT_THUMB, now); g_bHoldFired = true; }
    if (!R.btn2 && g_bDown && !g_bHoldFired && g_mono) { g_mono = false; LOG("controls: monocular away (weapon change)"); }
    if (!R.btn2 && g_bDown && !g_bHoldFired) { g_mapOpen = false; g_cycle = (g_cycle + 1) & 3; PulseBtn(kSlots[g_cycle], now); InterlockedExchange(&g_selectReq, 1); }
    g_bDown = R.btn2;
    for (auto& p : g_pulse) if (p.until > now) b |= p.btn;
    if (g_usePulse > now) b |= XINPUT_GAMEPAD_Y;           // a hand touched the interaction target
    g.wButtons = b;
    if (g_c.logZones && z != g_lastZoneLogged) { g_lastZoneLogged = z; LOG("controls: right hand zone %s", ZoneName(z)); }
}

static DWORD WINAPI Hook_GetState(DWORD idx, XINPUT_STATE* st) {
    if (!st) return ERROR_BAD_ARGUMENTS;
    if (idx != 0) return g_active ? ERROR_DEVICE_NOT_CONNECTED : o_GetState ? o_GetState(idx,st) : ERROR_DEVICE_NOT_CONNECTED;
    xr::HandState h[2]; XrPosef head; bool headValid = false;
    xr::InputStamp stamp;
    bool have = g_c.enable && xr::InputSnapshot(h, head, headValid, &stamp) && (h[0].active || h[1].active);
    if (!have) {
        const bool wasActive = InterlockedExchange(&g_active,0) != 0;
        if (wasActive || g_mappingGeneration) CancelPendingInput();
        if (wasActive) LOG("controls: VR input revoked; neutral release then real gamepad fallback");
        const DWORD native = o_GetState ? o_GetState(idx,st) : ERROR_DEVICE_NOT_CONNECTED;
        if (native == ERROR_SUCCESS || !wasActive) return native;
        g_last = {}; st->Gamepad = {}; st->dwPacketNumber = ++g_packet; return ERROR_SUCCESS;
    }
    if (!InterlockedExchange(&g_active, 1)) LOG("controls: VR controllers drive gamepad 0");
    if (g_mappingGeneration != stamp.generation) { CancelPendingInput(); g_mappingGeneration = stamp.generation; }
    if (g_inputCursor.Take(stamp)) {
        XINPUT_GAMEPAD g; Build(g,h,head,headValid);
        if (memcmp(&g,&g_last,sizeof g) != 0) { g_last = g; ++g_packet; }
    }
    st->dwPacketNumber = g_packet; st->Gamepad = g_last;
    return ERROR_SUCCESS;
}
static DWORD WINAPI Hook_SetState(DWORD idx, XINPUT_VIBRATION* v) {
    if (g_active && idx == 0) {
        if (v && g_c.rumble) xr::SetRumble(v->wLeftMotorSpeed / 65535.0f * g_c.rumble, v->wRightMotorSpeed / 65535.0f * g_c.rumble);
        return ERROR_SUCCESS;
    }
    return o_SetState ? o_SetState(idx, v) : ERROR_DEVICE_NOT_CONNECTED;
}

void Install(HMODULE dunia, const Config& c) {
    g_c = c; g_base = (uintptr_t)dunia;
    if (!c.enable) { LOG("controls: motion controls off"); return; }
    bool a = PatchIat(dunia, "XINPUT1_3.dll", 2, (void*)&Hook_GetState, (void**)&o_GetState);
    bool b = PatchIat(dunia, "XINPUT1_3.dll", 3, (void*)&Hook_SetState, (void**)&o_SetState);
    LOG("controls: XInputGetState hook %d, XInputSetState hook %d", a, b);
}
bool Active() { return g_active != 0; }
bool MacheteSelected() { return g_cycle == 3; }
bool MapOpen() { return g_mapOpen; }
void SetSightsAllowed(bool a) { g_sightsAllowed = a; }
bool RightHandInHolsterZone() { return g_zoneNow; }
bool SightsUp() { return g_active && g_sightOn; }
void PulseUse() { g_usePulse = GetTickCount64() + 120; }
void SetLeftGripClaimed(bool c) { g_leftClaimed = c; }

} // namespace controls
