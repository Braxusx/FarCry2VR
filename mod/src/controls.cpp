// controls.cpp - Quest/Touch motion controllers -> the game's own Xbox gamepad input.
// Dunia reads the pad through XInputGetState (xinput1_3 ordinal 2) and rumbles it through XInputSetState
// (ordinal 3). Both imports are redirected here: while VR controllers are active, pad 0 is synthesised from them,
// so the game's gamepad bindings, hints and menus all work unchanged.
#include "controls.h"
#include "arms.h"
#include "xr.h"
#include "log.h"
#include "gameaddr.h"
#include "common/arm_swing.h"
#include "common/motion_steering.h"
#include "common/native_axis_gate.h"
#include "common/native_sprint_gate.h"
#include "common/native_steering_response.h"
#include "common/native_gog_address.h"
#include "common/driving_controls.h"
#include <xinput.h>
#include <cmath>
#include <cstring>
#include <cstdio>

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
static inline uintptr_t NGA(uintptr_t va) { return fc2vr::native::GogImageAddress(g_base,gameaddr::Detect()==gameaddr::GOG,va); }
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
static volatile LONG g_passiveRide = 0;
void SetPassiveRide(bool riding) { InterlockedExchange(&g_passiveRide,riding ? 1 : 0); }
bool PassiveRide() { return g_vehMode == 0 && g_normalMap && !g_loadingMap && InterlockedCompareExchange(&g_passiveRide,0,0) != 0; }
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
unsigned long long PhoneAgeMs() { return g_phoneOut ? GetTickCount64() - g_phoneT : 0; }
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
static volatile bool g_menuMode = false; static volatile bool g_rClickBlocked = false;
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
static volatile float g_mcYaw = 0, g_mcPitch = 0; static volatile DWORD g_mcTick = 0; static volatile bool g_turretGrip = false;
bool TurretGripped() { return g_mounted && g_turretGrip; }
void SetMountedCamera(float yaw, float pitch) { g_mcYaw = yaw; g_mcPitch = pitch; g_mcTick = GetTickCount(); }
void SetRightClickBlocked(bool on) { g_rClickBlocked = on; }   // v234 test 83: the alignment menu ticks with the right stick click
struct MappingHistory {
    ULONGLONG recenterAt = 0, gadgetAt = 0, mapPress = 0, notMapSince = 0;
    bool recenterFired = false, driverMap = false, gadgetHeld = false, gadgetDelayed = false, mapPrev = false;
    int lastMap = -1, turnDir = 0, snapDir = 0;
    int turretHand = -1; float turretC0y = 0, turretC0p = 0, turretG0y = 0, turretG0p = 0; DWORD turretLog = 0;   // v234 test 88 motion aim
    bool aPrev = false, aCombo = false, rThrow = false, lThrow = false, trigFlash = false, bCombo = false;
};
static MappingHistory g_history;
static fc2vr::authority::SampleCursor g_inputCursor;
static uint64_t g_mappingGeneration = 0;
static fc2vr::motion::ArmSwingSprint g_armSprint;
static fc2vr::wheel::Steering g_wheel;
static bool g_swingSprintOn=false,g_wheelOn=false;
static uint8_t g_motionAxes=0,g_lastMotionAxes=0;
static SRWLOCK g_wheelHoldLock=SRWLOCK_INIT;
static xr::InputStamp g_wheelHoldSource;
static xr::InputStamp g_sprintSource;
static bool g_sprintRequest=false;
static uint8_t g_wheelHoldMask=0;
struct NativeSteeringOwner { uintptr_t pawn=0,vehicle=0,physics=0,havok=0,component=0,analog=0; };
static bool InitNativeSteeringOwner();
static NativeSteeringOwner ResolveSteeringOwner(void* pawn);
static bool g_steeringResponseEnabled=false;
static NativeSteeringOwner g_wheelOwner;
static bool g_wheelMotionOwned=false;
static float g_wheelDegrees=0,g_wheelEmittedAxis=0;
static uint64_t g_wheelHoldGeneration=0;
static void TraceWheelOwner(const NativeSteeringOwner& owner) {
    static ULONGLONG last=0;static unsigned reports=0;
    const auto now=GetTickCount64();
    if(!g_c.motionInputTrace || reports>=30 || (reports && now-last<2000)) return;
    last=now;++reports;
    LOG("motion-28 wheel owner: resolved=%d pawn=%p vehicle=%p physics=%p havok=%p component=%p analog=%p",
        owner.component!=0,(void*)owner.pawn,(void*)owner.vehicle,(void*)owner.physics,
        (void*)owner.havok,(void*)owner.component,(void*)owner.analog);
}
static void PublishWheelHands(const xr::InputStamp& source,uint8_t mask,float degrees=0,float axis=0,
    bool motionOwned=false,const NativeSteeringOwner& owner={}) {
    AcquireSRWLockExclusive(&g_wheelHoldLock);
    if(mask && !g_wheelHoldMask) ++g_wheelHoldGeneration;
    g_wheelHoldSource=source;g_wheelHoldMask=mask;
    g_wheelDegrees=degrees;g_wheelEmittedAxis=axis;
    g_wheelMotionOwned=motionOwned;g_wheelOwner=owner;
    ReleaseSRWLockExclusive(&g_wheelHoldLock);
}
static void PublishSprintRequest(const xr::InputStamp& source,bool requested) {
    AcquireSRWLockExclusive(&g_wheelHoldLock);
    g_sprintSource=source;g_sprintRequest=requested;
    ReleaseSRWLockExclusive(&g_wheelHoldLock);
}
bool MotionSprintCurrent() {
    if(g_c.moveDir!=1 || !g_c.armSwingSprint || !g_active || !g_normalMap || g_vehMode || g_mounted
        || g_menuMode || g_loadingMap || g_menuMap || PassiveRide() || ItemControls() || g_mapOpen) return false;
    AcquireSRWLockShared(&g_wheelHoldLock);
    const auto source=g_sprintSource;const bool requested=g_sprintRequest;
    ReleaseSRWLockShared(&g_wheelHoldLock);
    if(!requested) return false;
    xr::HandState hands[2];XrPosef head;bool headValid=false;xr::InputStamp current;
    if(!xr::InputSnapshot(hands,head,headValid,&current) || !headValid
        || hands[0].stick.y<=.65f || fabsf(hands[0].stick.x)>=.65f) return false;
    for(int side=0;side<2;++side) if(!hands[side].poseAuthority || !hands[side].gripValid
        || hands[side].squeeze>=.35f || hands[side].trigger>=.15f
        || !fc2vr::authority::Fresh(source,current,GetTickCount64(),side)) return false;
    return true;
}
static uint8_t CurrentWheelMask(const xr::InputStamp& source,uint8_t mask) {
    if(!g_c.motionSteering || !g_active || g_vehMode!=1 || g_menuMode || MapControls() || g_loadingMap || g_menuMap) return 0;
    if(!mask) return 0;
    xr::HandState hands[2];XrPosef head;bool headValid=false;xr::InputStamp current;
    if(!xr::InputSnapshot(hands,head,headValid,&current) || !headValid) return 0;
    for(int side=0;side<2;++side) if(!hands[side].poseAuthority || !hands[side].gripValid || hands[side].squeeze<.35f
        || !fc2vr::authority::Fresh(source,current,GetTickCount64(),side)) mask&=uint8_t(~(1u<<side));
    return mask;
}
uint8_t WheelHands() {
    AcquireSRWLockShared(&g_wheelHoldLock);
    const auto source=g_wheelHoldSource;const uint8_t mask=g_wheelHoldMask;
    ReleaseSRWLockShared(&g_wheelHoldLock);
    return CurrentWheelMask(source,mask);
}
bool WheelTraceInput(float& degrees,float& axis,uint64_t& holdGeneration) {
    degrees=axis=0;holdGeneration=0;
    if(!g_c.motionInputTrace) return false;
    AcquireSRWLockShared(&g_wheelHoldLock);
    const auto source=g_wheelHoldSource;const uint8_t mask=g_wheelHoldMask;
    const float angle=g_wheelDegrees,emitted=g_wheelEmittedAxis;
    const uint64_t hold=g_wheelHoldGeneration;
    ReleaseSRWLockShared(&g_wheelHoldLock);
    if(!CurrentWheelMask(source,mask) || !std::isfinite(angle) || !std::isfinite(emitted)) return false;
    degrees=angle;axis=emitted;holdGeneration=hold;return true;
}
static unsigned g_swingReports=0,g_wheelReports=0; // bounded diagnostics, not mapping state
struct MotionTraceWindow {
    ULONGLONG start=0;unsigned samples=0,eligible=0,active=0,zeroHost=0,blocked=0;
    float grip[2]{},trigger[2]{},request=0,sent=0;
};
static MotionTraceWindow g_motionTrace[2];
static unsigned g_motionTraceReports[2]{};
static ULONGLONG g_motionHost=0;
static uint64_t g_motionSequence=0;
struct SprintTraceWindow {
    ULONGLONG start=0;
    unsigned reason[static_cast<unsigned>(fc2vr::motion::ArmSwingReason::Count)]{},busy=0;
    float seconds=0,left=0,right=0,span=0,rawSpeed[2]{};
};
static SprintTraceWindow g_sprintTrace;
static unsigned g_sprintTraceReports=0;
struct WheelTraceWindow {
    ULONGLONG start=0;
    unsigned reason[static_cast<unsigned>(fc2vr::wheel::SteeringReason::Count)]{};
    float delta=0,gripMin[2]{1,1};
};
static WheelTraceWindow g_wheelTrace;
static unsigned g_wheelTraceReports=0;
static void TraceWheelDecision(ULONGLONG now,const fc2vr::wheel::SteeringDiagnostics& d,const xr::HandState* h) {
    if(!g_c.motionInputTrace || g_wheelTraceReports>=60) return;
    auto& w=g_wheelTrace;
    if(!w.start || now-w.start>5000) { w={};w.start=now; }
    const auto reason=static_cast<unsigned>(d.reason);
    if(reason<static_cast<unsigned>(fc2vr::wheel::SteeringReason::Count)) ++w.reason[reason];
    w.delta=fmaxf(w.delta,fabsf(d.deltaDegrees));
    for(int side=0;side<2;++side) w.gripMin[side]=fminf(w.gripMin[side],h[side].squeeze);
    if(now-w.start<2000) return;
    char reasons[384]{};size_t used=0;
    for(unsigned i=0;i<static_cast<unsigned>(fc2vr::wheel::SteeringReason::Count);++i) if(w.reason[i]) {
        const int count=std::snprintf(reasons+used,sizeof reasons-used,"%s%s:%u",used?",":"",
            fc2vr::wheel::SteeringReasonName(static_cast<fc2vr::wheel::SteeringReason>(i)),w.reason[i]);
        if(count<0 || size_t(count)>=sizeof reasons-used) break;
        used+=size_t(count);
    }
    LOG("motion-28 wheel hold: reasons=%s maxDelta=%.2fdeg gripMin=%.2f/%.2f",reasons,w.delta,w.gripMin[0],w.gripMin[1]);
    ++g_wheelTraceReports;w={};
}
static void TraceSprintDecision(ULONGLONG now,const fc2vr::motion::ArmSwingDiagnostics& d,unsigned busy) {
    if(!g_c.motionInputTrace || g_sprintTraceReports>=60) return;
    auto& w=g_sprintTrace;
    if(!w.start || now-w.start>5000) { w={};w.start=now; }
    const auto reason=static_cast<unsigned>(d.reason);
    if(reason<static_cast<unsigned>(fc2vr::motion::ArmSwingReason::Count)) ++w.reason[reason];
    w.busy|=busy;w.seconds=fmaxf(w.seconds,d.strokeSeconds);
    w.left=fmaxf(w.left,d.leftTravel);w.right=fmaxf(w.right,d.rightTravel);w.span=fmaxf(w.span,d.spanVariation);
    w.rawSpeed[0]=fmaxf(w.rawSpeed[0],d.rawLeftSpeed);w.rawSpeed[1]=fmaxf(w.rawSpeed[1],d.rawRightSpeed);
    if(now-w.start<2000) return;
    char reasons[512]{};size_t used=0;
    for(unsigned i=0;i<static_cast<unsigned>(fc2vr::motion::ArmSwingReason::Count);++i) if(w.reason[i]) {
        const int count=std::snprintf(reasons+used,sizeof(reasons)-used,"%s%s:%u",used?",":"",
            fc2vr::motion::ArmSwingReasonName(static_cast<fc2vr::motion::ArmSwingReason>(i)),w.reason[i]);
        if(count<0 || size_t(count)>=sizeof(reasons)-used) break;
        used+=size_t(count);
    }
    LOG("motion-28 sprint detector: reasons=%s busy=0x%02x maxStroke=%.3fs maxTravel=%.3f/%.3fm maxSpanChange=%.3fm rawSpeedMax=%.2f/%.2fmps",
        reasons,w.busy,w.seconds,w.left,w.right,w.span,w.rawSpeed[0],w.rawSpeed[1]);
    ++g_sprintTraceReports;w={};
}
static void TraceMotionInput(bool driving,ULONGLONG now,const xr::HandState* h,unsigned blocked,
    bool active,float request,float sent,bool zeroHost) {
    if(!g_c.motionInputTrace) return;
    const int side=driving?1:0;
    if(g_motionTraceReports[side]>=60) return;
    auto& w=g_motionTrace[side];
    if(!w.start || now-w.start>5000) { w={};w.start=now; }
    ++w.samples;if(!blocked) ++w.eligible;if(active) ++w.active;if(zeroHost) ++w.zeroHost;w.blocked|=blocked;
    for(int i=0;i<2;++i) { w.grip[i]=fmaxf(w.grip[i],h[i].squeeze);w.trigger[i]=fmaxf(w.trigger[i],h[i].trigger); }
    w.request=fmaxf(w.request,fabsf(request));w.sent=fmaxf(w.sent,fabsf(sent));
    if(now-w.start<2000) return;
    ++g_motionTraceReports[side];
    LOG("motion-28 %s input: samples=%u eligible=%u active=%u blocked=0x%03x hostClockZero=%u gripMax=%.2f/%.2f triggerMax=%.2f/%.2f requestMax=%.3f sentMax=%.3f",
        driving?"wheel":"sprint",w.samples,w.eligible,w.active,w.blocked,w.zeroHost,w.grip[0],w.grip[1],w.trigger[0],w.trigger[1],w.request,w.sent);
    w={};
}
void CancelPendingInput() {
    PublishWheelHands({},0);
    PublishSprintRequest({},false);g_motionAxes=0;
    g_history = {}; g_inputCursor.Clear(); g_mappingGeneration = 0;
    g_armSprint.Reset();g_wheel.Reset();g_swingSprintOn=g_wheelOn=false;
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
// v231 head-based movement: the body's forward in tracking space (set by the render thread every frame)
static volatile float g_bodyFx = 0, g_bodyFz = -1; static volatile LONG g_bodyValid = 0; static volatile float g_moveMag = 0;
void SetBodyForward(float x, float z, bool valid) { g_bodyFx = x; g_bodyFz = z; InterlockedExchange(&g_bodyValid, valid ? 1 : 0); }
float LastMoveStick() { return g_moveMag; }
// stick (x right, y forward) given in the head's frame -> the body's frame (what the game walks along)
static void StickToHead(float& sx, float& sy, const XrPosef& head) {
    const XrQuaternionf& q = head.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));   // head forward (0,0,-1) rotated, horizontal
    float n = sqrtf(fx * fx + fz * fz); if (n < 0.2f) return;   // looking straight up / down: keep the body's frame
    fx /= n; fz /= n;
    float bx = g_bodyFx, bz = g_bodyFz; float bn = sqrtf(bx * bx + bz * bz); if (bn < 0.5f) return; bx /= bn; bz /= bn;
    float rx = -bz, rz = bx;                 // the body's right (as the recenter's rightH)
    float hx = fx * rx + fz * rz, hy = fx * bx + fz * bz;   // the head's forward in the body frame (x right, y forward)
    float x = sx * hy + sy * hx, y = -sx * hx + sy * hy;
    sx = x; sy = y;
}
struct NativePadLease { XINPUT_STATE* pointer=nullptr;XINPUT_STATE value{};xr::InputStamp source{};uint8_t axes=0; };
static thread_local NativePadLease g_nativePad;
static void Build(XINPUT_GAMEPAD& g, const xr::HandState* h, const XrPosef& head, bool headValid, const xr::InputStamp& stamp) {
    memset(&g, 0, sizeof g);
    g_motionAxes=0;
    xr::HandState hm[2] = {h[0], h[1]};
    if (g_menuMode) { for (auto& x : hm) { x.stick.x = x.stick.y = 0; x.stickClick = false; } }   // the sticks drive the mod's menu
    else if (g_rClickBlocked) hm[1].stickClick = false;
    ULONGLONG now = GetTickCount64();
    UpdateVehicleMode();
    h = hm;
    const xr::HandState& L = h[0]; const xr::HandState& R = h[1];
    const V forward=Rot(head.orientation,{0,0,-1});
    // The tracked poses are sampled at XR predictedTime. GetTickCount64 is a
    // freshness/timeout clock and can repeat on adjacent 90 Hz input samples.
    const bool poseClock=stamp.predictedTime>0;
    const double sampleSeconds=double(stamp.predictedTime)*1e-9;
    const bool zeroHost=g_motionSequence && stamp.sequence!=g_motionSequence && now==g_motionHost;
    g_motionHost=now;g_motionSequence=stamp.sequence;
    fc2vr::wheel::Input wheel;
    const auto drivingButtons=fc2vr::input::DrivingLayout(g_c.motionSteering!=0,g_c.handbrakeA!=0,
        L.stickClick,R.btn1,L.squeeze,R.squeeze);
    wheel.allowed=g_c.motionSteering && g_vehMode==1 && !g_menuMode && !MapControls()
        && !g_loadingMap && !g_menuMap && !L.btn2 && !L.menu && !L.btn1 && !drivingButtons.changeSeat && poseClock;
    wheel.headTracked=headValid;wheel.seconds=sampleSeconds;wheel.sequence=stamp.sequence;wheel.generation=stamp.generation;
    wheel.head={head.position.x,head.position.y,head.position.z};wheel.forward={forward.x,forward.y,forward.z};
    wheel.fullLockDegrees=g_c.steeringLock;
    for(int side=0;side<2;++side) {
        wheel.tracked[side]=h[side].gripValid && h[side].poseAuthority;
        wheel.hand[side]={h[side].grip.position.x,h[side].grip.position.y,h[side].grip.position.z};
        wheel.squeeze[side]=h[side].squeeze;wheel.handGeneration[side]=stamp.handGeneration[side];
    }
    const auto steering=g_wheel.Update(wheel);
    if(g_vehMode==1) TraceWheelDecision(now,g_wheel.Diagnostics(),h);
    if(steering.engaged!=g_wheelOn) {
        if(g_wheelReports++<40) LOG("motion-28 wheel: %s reason=%s",steering.engaged?"two-hand wheel acquired":"released",
            fc2vr::wheel::SteeringReasonName(g_wheel.Diagnostics().reason));
        if(steering.engaged) { xr::Haptic(0,.25f,.04f);xr::Haptic(1,.25f,.04f); }
        g_wheelOn=steering.engaged;
    }
    // v222 recenter: the left stick held in for RecenterHoldMs (the click still sprints as usual)
    { auto& downAt = g_history.recenterAt; auto& fired = g_history.recenterFired;
      if (L.stickClick && (g_vehMode!=1 || drivingButtons.allowRecenter)) { if (!downAt) { downAt = now; fired = false; }
          else if (!fired && g_c.recenterHoldMs > 0 && now - downAt >= (ULONGLONG)g_c.recenterHoldMs) { fired = true; InterlockedExchange(&g_recenterReq, 1); xr::Haptic(0, 0.5f, 0.08f); LOG("controls: left stick held - recenter"); } }
      else downAt = 0; }
    WORD b = 0;
    const bool poseFire = g_vehMode != 0 || g_mounted || g_menuMap || (R.gripValid && R.aimValid);
    g_rtValue = poseFire ? R.trigger : 0;
    if (g_vehMode == 1 && !g_menuMode) {   // driver seat (VEHICLE_CONTROLS): the game's pad driving map, Config\InputActionMap.xml
        // Native driving actions: RT gas, LT brake/reverse, pad B handbrake,
        // pad A change seat, pad Y exit, pad LB heal, pad RB headlights.
        float sx = fabsf(R.stick.x) >= fabsf(L.stick.x) ? R.stick.x : L.stick.x;
        if(steering.engaged && fabsf(sx)<.2f) { sx=steering.axis;g_motionAxes=1; } // deliberate stick input overrides motion
        g_armSprint.Reset();g_swingSprintOn=false;
        PublishSprintRequest({},false);
        g.sThumbLX = Axis(sx); g.sThumbLY = Axis(L.stick.y);
        NativeSteeringOwner owner;
        if(g_steeringResponseEnabled && g_motionAxes==1) {
            owner=ResolveSteeringOwner(arms::LocalPawnIdentity());TraceWheelOwner(owner);
        }
        PublishWheelHands(stamp,steering.engaged?uint8_t((steering.held[0]?1u:0u)|(steering.held[1]?2u:0u)):0,
            steering.degrees,g.sThumbLX/32767.f,g_motionAxes==1,owner);
        g.bRightTrigger = (BYTE)lroundf(fminf(1.0f, R.trigger) * 255.0f);
        g.bLeftTrigger = (BYTE)lroundf(fminf(1.0f, L.trigger) * 255.0f);
        // Only a seat-change request cancels the wheel; either handbrake
        // layout retains the grip. Other driving actions are unchanged.
        if (drivingButtons.handbrake) b |= XINPUT_GAMEPAD_B;
        if (drivingButtons.changeSeat) b |= XINPUT_GAMEPAD_A;
        if (L.btn1) b |= XINPUT_GAMEPAD_Y;                         // exit / bail out
        if (R.stickClick) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;       // heal
        if (L.btn2) b |= XINPUT_GAMEPAD_BACK;                      // map
        { auto& prev = g_history.driverMap; if (L.btn2 && !prev) g_mapOpen = !g_mapOpen; prev = L.btn2; }
        if (L.menu) b |= XINPUT_GAMEPAD_START;
        if (R.btn2) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;            // B: headlights
        g_sightOn = false; g_zoneNow = false; g_rGripDown = R.squeeze > 0.6f; g_lGripDown = L.squeeze > 0.5f;
        for (auto& p : g_pulse) if (p.until > now) b |= p.btn;
        g.wButtons = b;
        const unsigned blocked=(!wheel.allowed?1u:0u) | (!headValid || !wheel.tracked[0] || !wheel.tracked[1]?2u:0u)
            | (!steering.engaged?4u:0u);
        TraceMotionInput(true,now,h,blocked,steering.engaged,steering.axis,g.sThumbLX/32767.f,zeroHost);
        return;
    }
    PublishWheelHands({},0);
    // left hand
    {   float sx = L.stick.x, sy = L.stick.y;
        if (g_c.moveDir == 1 && headValid && g_bodyValid) StickToHead(sx, sy, head);
        g.sThumbLX = Axis(sx); g.sThumbLY = Axis(sy); g_moveMag = sqrtf(sx * sx + sy * sy); }
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
        // v234 test 88: point to aim - with a grip held, the gun turns to follow where that controller points, relative
        // to how it was held when the grip went down. The game only takes stick input, so the stick is steered toward the
        // wanted heading from the gun's current one (the game camera on the gun), harder the further off it is.
        auto& mHand = g_history.turretHand; auto& c0y = g_history.turretC0y; auto& c0p = g_history.turretC0p;
        auto& g0y = g_history.turretG0y; auto& g0p = g_history.turretG0p; auto& lastLog = g_history.turretLog;
        const bool camOk = GetTickCount() - g_mcTick < 200;
        int want = !g_c.turretMotion || !camOk ? -1 : (R.squeeze > 0.6f && R.aimValid) ? 1 : (L.squeeze > 0.6f && L.aimValid) ? 0 : -1;
        if (mHand >= 0 && (want < 0 || !(mHand ? R : L).aimValid || (mHand ? R : L).squeeze < 0.4f)) { mHand = -1; LOG("controls: turret motion aim off"); }
        auto ang = [](const xr::HandState& h, float& yaw, float& pitch) {
            const XrQuaternionf q = h.aim.orientation;   // forward = q * (0, 0, -1) in the room (Y up)
            const float fx = -(2 * (q.x * q.z + q.w * q.y)), fy = -(2 * (q.y * q.z - q.w * q.x)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
            yaw = atan2f(fx, -fz); pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fy)));
        };
        if (mHand < 0 && want >= 0) {
            mHand = want; ang(mHand ? R : L, c0y, c0p); g0y = g_mcYaw; g0p = g_mcPitch;
            LOG("controls: turret motion aim on (%s hand), gun heading %.1f pitch %.1f", mHand ? "right" : "left", g0y * 57.2958f, g0p * 57.2958f);
        }
        g_turretGrip = mHand >= 0;
        if (mHand >= 0) {
            float cy, cp; ang(mHand ? R : L, cy, cp);
            auto wrap = [](float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
            // the room turns right with yaw up; the game's heading turns left with yaw up (Z up)
            // v234 test 90: like the gun's rear handles - the controller down raises the barrel (TurretMotionInvertY=1, default)
            const float wy = g0y - wrap(cy - c0y), wp = fmaxf(-1.4f, fminf(1.4f, g0p + (g_c.turretMotionInvY ? -1.0f : 1.0f) * (cp - c0p)));
            const float ey = wrap(wy - g_mcYaw) * 57.2958f, ep = (wp - g_mcPitch) * 57.2958f;   // degrees still to go
            auto drive = [](float e) {   // past the game's stick dead zone at once, full stick 8 deg off
                const float a = fabsf(e); if (a < 0.25f) return 0.0f;
                return (e > 0 ? 1.0f : -1.0f) * fminf(1.0f, 0.28f + 0.72f * a / 8.0f);
            };
            g.sThumbRX = Axis(-drive(ey));   // right on the stick turns the heading clockwise (down)
            g.sThumbRY = Axis(drive(ep));
            if (GetTickCount() - lastLog > 1000) { lastLog = GetTickCount(); DLOG("controls: turret motion aim - off by %.1f deg sideways, %.1f deg up/down", ey, ep); }
        }
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
    // no holster grabs in a vehicle seat, nor in menus / loading screens (v234 test 36: a chest grab in the pause menu left
    // the mod thinking the machete was out - the gun in the hand then got the machete's hand angles, twisted)
    const bool inMenu = g_menuMap || g_loadingMap;
    Zone z = headValid && g_vehMode == 0 && !inMenu ? RightHandZone(R, head) : Z_NONE;
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
    if (g_vehMode == 2 && grip && !(g_mounted && g_c.turretMotion)) { b |= XINPUT_GAMEPAD_A; g_rGripAim = false; }   // passenger seat: right grip changes seat (v234 test 89: not on a mounted gun with motion aim - the grip aims it there; A still changes seat)
    // iron sight / scope: hold the weapon up to your face (CrysisVR: on within 0.25 m of the head, off beyond 0.35 m)
    if (headValid && R.gripValid) {
        float dx = R.grip.position.x - head.position.x, dy = R.grip.position.y - head.position.y, dz = R.grip.position.z - head.position.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        bool was = g_sightOn;
        g_sightOn = !g_mounted && (g_leftClaimed || !g_c.sightNeedsGrip) && d < (g_sightOn ? g_c.sightOff : g_c.sightOn);   // v234 test 91: never on a mounted gun (its zoom opened the scope window)
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
    if (R.btn2 && !g_bHoldFired && now - g_bTime >= (ULONGLONG)g_c.holdMs && !inMenu) { PulseBtn(XINPUT_GAMEPAD_RIGHT_THUMB, now); g_bHoldFired = true; }
    if (!R.btn2 && g_bDown && !g_bHoldFired && g_mono) { g_mono = false; LOG("controls: monocular away (weapon change)"); }
    if (!R.btn2 && g_bDown && !g_bHoldFired && !inMenu) { g_mapOpen = false; g_cycle = (g_cycle + 1) & 3; PulseBtn(kSlots[g_cycle], now); InterlockedExchange(&g_selectReq, 1); }   // B in a menu = back, not a weapon change
    g_bDown = R.btn2;
    for (auto& p : g_pulse) if (p.until > now) b |= p.btn;
    if (g_usePulse > now) b |= XINPUT_GAMEPAD_Y;           // a hand touched the interaction target
    fc2vr::motion::ArmSwingInput swing;
    const unsigned sprintBusy=(g_sightOn?1u:0u) | (g_crouchDown?2u:0u) | (g_leftClaimed?4u:0u)
        | (g_aimHand>=0?8u:0u) | (arms::MotionGestureBusy()?16u:0u) | (arms::HeldMelee()!=0?32u:0u);
    // Grouped gate reasons are logged only during forward-stick intent. They
    // distinguish a rejected gesture from a correctly emitted native button.
    const unsigned sprintBlocked=
        ((!g_c.armSwingSprint || !g_normalMap || g_loadingMap || g_menuMode || g_menuMap || g_vehMode!=0 || g_mounted || PassiveRide())?1u:0u)
        | (!(L.stick.y>.65f && fabsf(L.stick.x)<.65f)?2u:0u)
        | ((!headValid || !wheel.tracked[0] || !wheel.tracked[1])?4u:0u)
        | ((ItemControls() || g_mapOpen || g_mono || WatchActive())?8u:0u)
        | (sprintBusy?16u:0u)
        | ((L.trigger>=.15f || R.trigger>=.15f)?32u:0u)
        | ((L.squeeze>=.35f || R.squeeze>=.35f)?64u:0u)
        | ((L.btn1 || L.btn2 || L.menu || R.btn1 || R.btn2)?128u:0u) | (!poseClock?256u:0u);
    swing.allowed=sprintBlocked==0;
    swing.tracked=headValid && wheel.tracked[0] && wheel.tracked[1];
    swing.seconds=wheel.seconds;swing.sequence=stamp.sequence;swing.generation=stamp.generation;
    swing.head=wheel.head;swing.forward=wheel.forward;
    for(int side=0;side<2;++side) { swing.hand[side]=wheel.hand[side];swing.handGeneration[side]=stamp.handGeneration[side]; }
    const bool sprint=g_armSprint.Update(swing);
    if(L.stick.y>.4f) TraceSprintDecision(now,g_armSprint.Diagnostics(),sprintBusy);
    PublishSprintRequest(stamp,sprint);
    if(sprint && g_c.moveDir==1) g_motionAxes=3;
    if(sprint) b|=XINPUT_GAMEPAD_LEFT_THUMB;
    if(sprint!=g_swingSprintOn) {
        if(g_swingReports++<60) {
            const auto d=g_armSprint.Diagnostics();
            LOG("motion-28 arm sprint: %s reason=%s busy=0x%02x stroke=%.3fs travel=%.3f/%.3fm span=%.3fm rawSpeed=%.2f/%.2fmps",
                sprint?"requested":"released",fc2vr::motion::ArmSwingReasonName(d.reason),sprintBusy,
                d.strokeSeconds,d.leftTravel,d.rightTravel,d.spanVariation,d.rawLeftSpeed,d.rawRightSpeed);
        }
        g_swingSprintOn=sprint;
    }
    g.wButtons = b;
    if(L.stick.y>.4f) TraceMotionInput(false,now,h,sprintBlocked,sprint,sprint?1.f:0.f,
        (b&XINPUT_GAMEPAD_LEFT_THUMB)?1.f:0.f,zeroHost);
    if (g_c.logZones && z != g_lastZoneLogged) { g_lastZoneLogged = z; LOG("controls: right hand zone %s", ZoneName(z)); }
}

static DWORD WINAPI Hook_GetState(DWORD idx, XINPUT_STATE* st) {
    g_nativePad={};
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
        XINPUT_GAMEPAD g; Build(g,h,head,headValid,stamp);
        if (memcmp(&g,&g_last,sizeof g) != 0 || g_motionAxes!=g_lastMotionAxes) { g_last = g; ++g_packet; }
        g_lastMotionAxes=g_motionAxes;
    }
    st->dwPacketNumber = g_packet; st->Gamepad = g_last;
    g_nativePad={st,*st,stamp,g_motionAxes};
    return ERROR_SUCCESS;
}
static int __cdecl MotionAxisThreshold(uint8_t* driver,unsigned axis) {
    const auto& lease=g_nativePad;
    if(!fc2vr::input::MotionAxisOwned(axis,lease.axes,true,true) || !driver
        || driver+0x14!=(uint8_t*)lease.pointer || IsBadReadPtr(driver+0x0c,0x18)) return false;
    const bool matched=*(const uint32_t*)(driver+0x0c)==0 && !memcmp(driver+0x14,&lease.value,sizeof lease.value);
    const bool current=(lease.axes==1 ? WheelHands()!=0 : MotionSprintCurrent())
        && xr::InputCurrent(lease.source,0) && xr::InputCurrent(lease.source,1);
    const bool bypass=fc2vr::input::MotionAxisOwned(axis,lease.axes,matched,current);
    if(bypass) { static unsigned reports=0;if(reports++<12) LOG("motion-28 native axis: dead-zone bypass axis=%u mask=%u packet=%u",axis,(unsigned)lease.axes,(unsigned)lease.value.dwPacketNumber); }
    return bypass;
}
static void InstallMotionAxisGate(PatchFn patch) {
    if(gameaddr::Detect()!=gameaddr::GOG || !patch) return;
    const uintptr_t site=g_base+0x2c786d;
    static const uint8_t expected[]={0xf3,0x0f,0x10,0x51,0x08,0x56};
    if(IsBadReadPtr((void*)site,sizeof expected) || memcmp((void*)site,expected,sizeof expected)) {
        LOG("motion-28 native axis: byte guard mismatch; native dead zone retained");return;
    }
    auto stub=(uint8_t*)VirtualAlloc(nullptr,96,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!stub) return;
    const auto code=fc2vr::input::MakeAxisGateStub((uint32_t)(uintptr_t)stub,(uint32_t)(uintptr_t)&MotionAxisThreshold,(uint32_t)(site+sizeof expected));
    memcpy(stub,code.bytes.data(),code.size);DWORD old=0;
    if(!VirtualProtect(stub,96,PAGE_EXECUTE_READ,&old)) { VirtualFree(stub,0,MEM_RELEASE);return; }
    FlushInstructionCache(GetCurrentProcess(),stub,code.size);
    void* trampoline=nullptr;
    const bool installed=patch(site,expected,sizeof expected,stub,&trampoline);
    if(!installed) VirtualFree(stub,0,MEM_RELEASE);
    LOG("motion-28 native axis: scoped motion threshold hook %d (native saturation/events retained)",installed);
}
static DWORD WINAPI Hook_SetState(DWORD idx, XINPUT_VIBRATION* v) {
    if (g_active && idx == 0) {
        if (v && g_c.rumble) xr::SetRumble(v->wLeftMotorSpeed / 65535.0f * g_c.rumble, v->wRightMotorSpeed / 65535.0f * g_c.rumble);
        return ERROR_SUCCESS;
    }
    return o_SetState ? o_SetState(idx, v) : ERROR_DEVICE_NOT_CONNECTED;
}

#include "controls_sprint.inl"
#include "controls_vehicle_owner.inl"
#include "controls_steering.inl"
void Install(HMODULE dunia, const Config& c,PatchFn patch) {
    g_c = c; g_base = (uintptr_t)dunia;
    LOG("motion-28 input: pose-sample clock; MotionSteering=%d SteeringFullLockDegrees=%.1f ArmSwingSprint=%d MotionInputTrace=%d",c.motionSteering,c.steeringLock,c.armSwingSprint,c.motionInputTrace);
    if (!c.enable) { LOG("controls: motion controls off"); return; }
    if(c.motionSteering || c.armSwingSprint) InstallMotionAxisGate(patch);
    InstallMotionSprintGate(dunia,patch);
    InstallMotionSteeringResponse(patch);
    bool a = PatchIat(dunia, "XINPUT1_3.dll", 2, (void*)&Hook_GetState, (void**)&o_GetState);
    bool b = PatchIat(dunia, "XINPUT1_3.dll", 3, (void*)&Hook_SetState, (void**)&o_SetState);
    LOG("controls: XInputGetState hook %d, XInputSetState hook %d", a, b);
}
bool Active() { return g_active != 0; }
bool MacheteSelected() {   // v234 test 70: the held weapon decides when it is known; the slot guess only before that
    const int m = arms::HeldMelee();
    if (m >= 0) {
        if (m == 1 && g_cycle != 3) g_cycle = 3;
        if (m == 0 && g_cycle == 3) { LOG("controls: slot guess said machete, the held weapon is not - guess corrected"); g_cycle = 0; }
        return m == 1;
    }
    return g_cycle == 3;
}
bool SecondarySelected() { return g_cycle == 1; }   // the hip slot (pistols, SMGs, sawed-off, M-79, flare gun, IED)
bool MapOpen() { return g_mapOpen; }
void SetSightsAllowed(bool a) { g_sightsAllowed = a; }
bool RightHandInHolsterZone() { return g_zoneNow; }
bool SightsUp() { return g_active && g_sightOn; }
void PulseUse() { g_usePulse = GetTickCount64() + 120; }
void SetLeftGripClaimed(bool c) { g_leftClaimed = c; }

} // namespace controls
