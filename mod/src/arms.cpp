// arms.cpp - first-person weapon and arms follow the right motion controller; shots follow the controller.
//
// Far Cry 2's first-person arms are the player pawn's own skeleton, animated relative to its "Camera" bone; the
// weapon entity is attached to that skeleton. After the engine computes the skeleton's model-space bone matrices
// (hierarchy pass 0x103322C0, or the job version 0x1064EC50), every bone of both arm chains (clavicles down to the
// fingers) is moved rigidly so that the rig, as animated in front of the Camera bone, is re-posed in front of the
// controller: camera frame -> controller aim frame, with the right hand landing on the controller grip.
// The player's bullet origin/direction (0x10112600, the player branch of the bullet strategy) are replaced by the
// controller's aim ray (keeping the game's spread).
#include "arms.h"
#include "log.h"
#include "gameaddr.h"
#include "xr.h"
#include "controls.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace arms {

static uintptr_t g_base = 0;
static inline uintptr_t A(uintptr_t va) { uint32_t v = gameaddr::Xlate((uint32_t)va); return v ? g_base + (v - 0x10000000) : 0; }
static Config g_c;

// ---------------------------------------------------------------- math (row-vector convention, as the engine)
struct M4 { float m[4][4]; };
static M4 Mul(const M4& a, const M4& b) {
    M4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; } return r;
}
static bool Inv(const M4& a, M4& out) {   // general 4x4 inverse (Gauss-Jordan)
    float t[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { t[i][j] = a.m[i][j]; t[i][j + 4] = i == j ? 1.0f : 0.0f; }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabsf(t[r][c]) > fabsf(t[p][c])) p = r;
        if (fabsf(t[p][c]) < 1e-9f) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { float x = t[c][j]; t[c][j] = t[p][j]; t[p][j] = x; }
        float d = 1.0f / t[c][c]; for (int j = 0; j < 8; ++j) t[c][j] *= d;
        for (int r = 0; r < 4; ++r) if (r != c) { float f = t[r][c]; if (f != 0) for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out.m[i][j] = t[i][j + 4];
    return true;
}
static M4 FromRows(const float* r, const float* u, const float* b, const float* o) {
    M4 m{}; for (int k = 0; k < 3; ++k) { m.m[0][k] = r[k]; m.m[1][k] = u[k]; m.m[2][k] = b[k]; m.m[3][k] = o[k]; }
    m.m[3][3] = 1; return m;
}
static void XformPt(const M4& m, const float* p, float* o) { for (int k = 0; k < 3; ++k) o[k] = p[0] * m.m[0][k] + p[1] * m.m[1][k] + p[2] * m.m[2][k] + m.m[3][k]; }
static void XformDir(const M4& m, const float* p, float* o) { for (int k = 0; k < 3; ++k) o[k] = p[0] * m.m[0][k] + p[1] * m.m[1][k] + p[2] * m.m[2][k]; }
static void Normalize(float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float Dot3(const float* a, fc2vr::tracking::Vec3 b) { return a[0]*b.x + a[1]*b.y + a[2]*b.z; }
static fc2vr::tracking::Vec3 Read3(const float* a) { return {a[0],a[1],a[2]}; }
static void Put3(float* out, fc2vr::tracking::Vec3 v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

// ---------------------------------------------------------------- shared state
static CRITICAL_SECTION g_cs; static bool g_csInit = false;
static Frame g_frame;                               // latest from the render thread
static FrameUsage g_frameUsage;
static volatile uint8_t* g_playerSkel = nullptr;    // player's skeleton object (bone array +0x20, count +0x2C)
static float g_dgW[3], g_dgHand[3], g_dgTarget[3]; static long g_dgSeq = 0; static float g_dgForeStep = 0; static float g_dgModel[512][16]; static bool g_dgWritten[512]; static int g_dgN = 0; static uint8_t* g_dgSkel = nullptr; static float g_dgArmPos[10], g_dgArmRot[10];   // v231 jitter diagnostic
static volatile uint8_t* g_playerGfx = nullptr;     // its graphic component (world matrix at +0x170)
static int g_slot[21];                              // bone-helper slot -> bone index (Camera 0, R Hand 12, R Clavicle 15, L Clavicle 19)
struct PhoneDiag { volatile LONG calls, player, applied, noFrame, noHand, camUpd; void* other[4]; int otherN[4]; LONG otherCalls[4]; };
static PhoneDiag g_pd = {};
static float g_camBoneRot[3][3]; static float g_camBonePos[3]; static volatile LONG g_camBoneSeq = 0;
static float g_perm[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; static bool g_permOk = false;   // camera-bone axes -> game camera rows
static bool g_dumped = false;
static uint8_t g_armMask[512]; static int g_maskCount = -1; static uint8_t* g_maskSkel = nullptr;
static volatile LONG g_applied = 0, g_shots = 0;
static float g_scopeO[3], g_scopeD[3]; static DWORD g_scopeTick = 0;
static fc2vr::authority::Stamp g_scopeSource; static void* g_scopeWeapon = nullptr;
// Reloads: the weapon reload state (0x10166xxx) starts the "reload" animation at 0x10166501 (edi = state object,
// [state+0xC] = owner, owner -> pawn via 0x100EA6C0, [state+5] set to 1 by the "reload_done" animation event,
// 0x10165800). A detour there notes the player's reloads; while one runs, the arms keep the pose they had before it.
static uint8_t* g_pawn = nullptr;   // the player's pawn (set by the per-frame update)
static volatile ULONGLONG g_diagUntil = 0;
static uint8_t* volatile g_wGfx = nullptr; static uint8_t* volatile g_wSkel = nullptr;   // the held weapon's graphic component (world matrix +0x170) and its skeleton   // weapon attachment diagnostic window (after a projectile shot)
static void* volatile g_reloadState = nullptr; static volatile DWORD g_reloadT = 0;

static float g_lastSpan = 0; static float g_rayRefO[3], g_rayRefD[3]; static DWORD g_rayTick = 0;
static float g_foregripRef[3]; static bool g_foregripValid = false, g_oneHanded = true;
static float g_shotO[3], g_shotF[3], g_camFwdNow[3], g_camRefO[3], g_camRefD[3]; static bool g_shotValid = false;
static fc2vr::authority::Stamp g_shotSource; static void* g_shotWeapon = nullptr;


// v231 rotation helpers (row-vector matrices: rows 0..2 are the basis vectors)
static void MatQuat(const M4& M, float* q) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) { float l = sqrtf(M.m[i][0] * M.m[i][0] + M.m[i][1] * M.m[i][1] + M.m[i][2] * M.m[i][2]); if (l < 1e-8f) l = 1; for (int j = 0; j < 3; ++j) r[i][j] = M.m[i][j] / l; }
    float t = r[0][0] + r[1][1] + r[2][2];
    if (t > 0) { float s = sqrtf(t + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (r[1][2] - r[2][1]) / s; q[1] = (r[2][0] - r[0][2]) / s; q[2] = (r[0][1] - r[1][0]) / s; }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) { float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2; q[3] = (r[1][2] - r[2][1]) / s; q[0] = 0.25f * s; q[1] = (r[1][0] + r[0][1]) / s; q[2] = (r[2][0] + r[0][2]) / s; }
    else if (r[1][1] > r[2][2]) { float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2; q[3] = (r[2][0] - r[0][2]) / s; q[0] = (r[1][0] + r[0][1]) / s; q[1] = 0.25f * s; q[2] = (r[2][1] + r[1][2]) / s; }
    else { float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2; q[3] = (r[0][1] - r[1][0]) / s; q[0] = (r[2][0] + r[0][2]) / s; q[1] = (r[2][1] + r[1][2]) / s; q[2] = 0.25f * s; }
}
static M4 QuatMat(const float* q) {
    float x = q[0], y = q[1], z = q[2], w = q[3]; M4 M{};
    M.m[0][0] = 1 - 2 * (y * y + z * z); M.m[0][1] = 2 * (x * y + z * w); M.m[0][2] = 2 * (x * z - y * w);
    M.m[1][0] = 2 * (x * y - z * w); M.m[1][1] = 1 - 2 * (x * x + z * z); M.m[1][2] = 2 * (y * z + x * w);
    M.m[2][0] = 2 * (x * z + y * w); M.m[2][1] = 2 * (y * z - x * w); M.m[2][2] = 1 - 2 * (x * x + y * y);
    M.m[3][3] = 1; return M;
}
static void QuatNlerp(const float* a, const float* b, float k, float* o) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sg = d < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 4; ++i) o[i] = a[i] * (1 - k) + b[i] * sg * k;
    float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]); if (l < 1e-8f) l = 1; for (int i = 0; i < 4; ++i) o[i] /= l;
}
struct Bone { uint32_t hash; uint8_t pad0[12]; float local[16]; float model[16]; int parent; uint8_t locked; uint8_t pad1[11]; };
static_assert(sizeof(Bone) == 0xA0, "bone stride");

static void Lock() { EnterCriticalSection(&g_cs); }
static void Unlock() { LeaveCriticalSection(&g_cs); }

bool ReadFrameUsage(FrameUsage& out) {
    if (!g_csInit) return false;
    Lock(); out = g_frameUsage; Unlock(); return true;
}

void SubmitFrame(const Frame& f) {
    if (!g_csInit) return;
    Lock(); g_frame = f; Unlock();
    if (!f.valid || !f.handAuthority) { Lock(); g_shotValid = false; g_rayTick = 0; g_scopeTick = 0; Unlock(); }
    // Learn how the Camera bone's axes map onto the game camera's rows (right, forward, up) once both are known.
    if (!g_permOk && f.valid && g_camBoneSeq > 30) {
        float B[3][3]; Lock(); memcpy(B, g_camBoneRot, sizeof B); Unlock();
        float P[3][3]; bool ok = true;
        for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) { P[i][k] = Dot3(f.camRows[i], B[k]); }
        for (int i = 0; i < 3; ++i) { int big = 0; for (int k = 0; k < 3; ++k) if (fabsf(P[i][k]) > 0.97f) ++big; if (big != 1) ok = false; }
        static int tries = 0;
        if (ok) {
            for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) g_perm[i][k] = fabsf(P[i][k]) > 0.97f ? (P[i][k] > 0 ? 1.0f : -1.0f) : 0.0f;
            g_permOk = true;
            LOG("arms: camera bone axes -> camera rows: [%g %g %g] [%g %g %g] [%g %g %g]", g_perm[0][0], g_perm[0][1], g_perm[0][2],
                g_perm[1][0], g_perm[1][1], g_perm[1][2], g_perm[2][0], g_perm[2][1], g_perm[2][2]);
        } else if (++tries % 900 == 1) {
            LOG("arms: camera bone vs camera rows not aligned yet: [%.2f %.2f %.2f] [%.2f %.2f %.2f] [%.2f %.2f %.2f]",
                P[0][0], P[0][1], P[0][2], P[1][0], P[1][1], P[1][2], P[2][0], P[2][1], P[2][2]);
        }
    }
}


static void HandInteract(uint8_t* pawn, uint8_t* pd);
// ---------------------------------------------------------------- player skeleton discovery (camera component update)
typedef void (__fastcall *CamUpdate_t)(void* self, void* edx, uint32_t a1, uint32_t a2);   // thiscall, ret 8
static CamUpdate_t o_CamUpdate = nullptr;
typedef void* (__fastcall *GetPawn_t)(void* comp);
typedef void* (__fastcall *GetSkel_t)(void* gfx, void* edx);
static void __fastcall Hook_CamUpdate(void* self, void* edx, uint32_t a1, uint32_t a2) {
    o_CamUpdate(self, edx, a1, a2);
    void* pawn = ((GetPawn_t)A(0x106933E0))((uint8_t*)self - 4);
    if (!pawn) return;
    uint8_t* pd = *(uint8_t**)((uint8_t*)pawn + 0x10);
    if (!pd) return;
    uint8_t* helper = pd + 4;
    uint8_t* gfx = *(uint8_t**)(helper + 8);
    if (!gfx || !*(void**)(helper + 4)) return;
    void** vt = *(void***)gfx;
    uint8_t* skel = (uint8_t*)((GetSkel_t)vt[0x78 / 4])(gfx, nullptr);
    if (!skel) return;
    for (int i = 0; i < 21; ++i) g_slot[i] = *(int*)(helper + 0x10 + i * 4);
    InterlockedIncrement(&g_pd.camUpd);
    HandInteract((uint8_t*)pawn, pd);
    if (g_c.noIdleBreak) { *(float*)(pd + 0x4B4) = 0; *(float*)(pd + 0x4B8) = 0; *(float*)(pd + 0x4BC) = 0; }   // IdleCycleBreaker min/max/countdown: 0 = never
    if (skel != g_playerSkel) LOG("arms: player skeleton %p (gfx %p), %d bones; Camera=%d RHand=%d RClav=%d LClav=%d LHand=%d",
                                  skel, gfx, *(int*)(skel + 0x2C), g_slot[0], g_slot[12], g_slot[15], g_slot[19], g_slot[20]);
    g_playerGfx = gfx; g_playerSkel = skel;
}

// ---------------------------------------------------------------- arm re-pose
static int g_follow[512];   // non-arm helper bone -> arm bone it sits on (or -1)
static M4 g_folAnim[512];   // helper bones as animated this frame, in world (held during reloads)
static void BuildMask(uint8_t* skel, Bone* bones, int n) {
    memset(g_armMask, 0, sizeof g_armMask);
    for (int i = 0; i < 512; ++i) g_follow[i] = -1;
    int roots[2] = { g_slot[15], g_slot[19] };
    for (int i = 0; i < n && i < 512; ++i) {
        for (int j = i, guard = 0; j >= 0 && j < n && guard < 64; j = bones[j].parent, ++guard)
            if (j == roots[0] || j == roots[1]) { g_armMask[i] = 1; break; }
    }
    if (g_slot[0] >= 0 && g_slot[0] < 512) g_armMask[g_slot[0]] = 0;   // never move the camera
    g_maskCount = n; g_maskSkel = skel;
    int c = 0; for (int i = 0; i < n && i < 512; ++i) c += g_armMask[i];
    LOG("arms: %d of %d bones belong to the arm chains", c, n);
    // helper bones outside the arm chains that sit on an arm joint (e.g. elbow/sleeve deformers parented to the root)
    for (int i = 0; i < n && i < 512; ++i) {
        if (g_armMask[i] || i == g_slot[0]) continue;
        for (int j = 0; j < n && j < 512; ++j) {
            if (!g_armMask[j]) continue;
            float dx = bones[i].model[12] - bones[j].model[12], dy = bones[i].model[13] - bones[j].model[13], dz = bones[i].model[14] - bones[j].model[14];
            if (dx * dx + dy * dy + dz * dz < 1e-6f) { g_follow[i] = j; LOG("arms: bone %d follows arm bone %d", i, j); break; }
        }
    }
}
static void Dump(Bone* bones, int n) {
    LOG("arms: player skeleton bone list (index hash parent model-pos):");
    for (int i = 0; i < n; ++i)
        LOG("  bone %3d %08x parent %3d  %8.3f %8.3f %8.3f%s", i, bones[i].hash, bones[i].parent, bones[i].model[12], bones[i].model[13], bones[i].model[14], g_armMask[i] ? " arm" : "");
}

static float Dist(const float* a, const float* b) { float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; return sqrtf(Dot3(d, d)); }
static M4 Translate(float x, float y, float z) { M4 m{}; m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1; m.m[3][0] = x; m.m[3][1] = y; m.m[3][2] = z; return m; }
// rotation by `ang` about unit axis, for row vectors (v' = v * R)
static M4 RotMat(const float* ax, float ang) {
    float c = cosf(ang), s = sinf(ang), t = 1 - c, x = ax[0], y = ax[1], z = ax[2];
    M4 m{}; m.m[3][3] = 1;
    // column-vector matrix R; row-vector form is its transpose
    float R[3][3] = { { t * x * x + c, t * x * y - s * z, t * x * z + s * y },
                      { t * x * y + s * z, t * y * y + c, t * y * z - s * x },
                      { t * x * z - s * y, t * y * z + s * x, t * z * z + c } };
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m.m[i][j] = R[j][i];
    return m;
}
// bone frame `oldM` whose segment ran p0->p1 is turned (shortest arc) to run q0->q1, with its origin moved to q0
static M4 AimSegment(const M4& oldM, const float* p0, const float* p1, const float* q0, const float* q1) {
    float u[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, v[3] = { q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2] };
    Normalize(u); Normalize(v);
    float ax[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float sn = sqrtf(Dot3(ax, ax)), cs = Dot3(u, v);
    M4 r = oldM;
    if (sn > 1e-6f) {
        for (int k = 0; k < 3; ++k) ax[k] /= sn;
        M4 R = RotMat(ax, atan2f(sn, cs));
        for (int i = 0; i < 3; ++i) { float row[3] = { oldM.m[i][0], oldM.m[i][1], oldM.m[i][2] }, o[3]; XformDir(R, row, o); r.m[i][0] = o[0]; r.m[i][1] = o[1]; r.m[i][2] = o[2]; }
    }
    r.m[3][0] = q0[0]; r.m[3][1] = q0[1]; r.m[3][2] = q0[2];
    return r;
}
// apply D (world, row-vector) to bone `root` and all its descendants
static void SubtreeXform(M4* Bw, Bone* bones, int n, int root, const M4& D) {
    for (int i = 0; i < n && i < 512; ++i) {
        if (!g_armMask[i]) continue;
        for (int j = i, guard = 0; j >= 0 && j < n && guard < 64; j = bones[j].parent, ++guard)
            if (j == root) { Bw[i] = Mul(Bw[i], D); break; }
    }
}

static M4 g_relR, g_relL; static volatile bool g_relHave = false; static bool g_relCached = false;
static volatile uint32_t g_weaponKey = 0;
static volatile uint32_t g_typeKey = 0;   // v208: the hold key - reticle id + inventory slot + model bone count (the reticle alone is shared by most guns)
static bool g_freeLDirty = false;   // the empty-hands hold was taken again: reload the free left hand pose
static M4 g_prevFinalRh; static bool g_havePrevFinal = false;    // last frame's final right hand (world)
static M4 g_off0; static bool g_haveOff0 = false;               // the weapon's place in the hand when the hold was taken
static M4 g_foreRel[2]; static bool g_haveForeRel[2] = { false, false };   // forearm relative to the hand in a normal hold
static bool g_offBroken = false;                                  // the weapon is not rigid in the hand: correction off
static bool WeaponWorld(void* w, M4& out) {
    if (!w || IsBadReadPtr((uint8_t*)w + 8, 4)) return false;
    uint8_t* wh = *(uint8_t**)((uint8_t*)w + 8); if (!wh || IsBadReadPtr(wh, 16)) return false;
    uint8_t* ent = *(uint8_t**)(wh + 12); if (!ent || IsBadReadPtr(ent + 0x30, 64)) return false;
    memcpy(out.m, ent + 0x30, 64); out.m[0][3] = out.m[1][3] = out.m[2][3] = 0; out.m[3][3] = 1;
    return std::isfinite(out.m[0][0]) && std::isfinite(out.m[3][0]);
}          // current weapon type (HUD reticle name id), from main
static int g_curAxis = -1; static float g_curSign = 1;   // the current weapon type's barrel axis (weapon entity row), -1 = unknown
static char g_ini[MAX_PATH] = "";
static bool LoadHold(uint32_t key, M4& r, M4& l, int* axis, float* sign) {
    if (!g_ini[0]) return false;
    char k[16], v[1024]; snprintf(k, sizeof k, "H%08X", key);
    GetPrivateProfileStringA("Holds", k, "", v, sizeof v, g_ini);
    float x[32]; int n = 0; const char* p = v;
    while (*p && n < 32) { char* e; float f = strtof(p, &e); if (e == p) break; x[n++] = f; p = e; while (*p == ' ' || *p == ',') ++p; }
    if (n != 32) return false;
    memcpy(r.m, x, 64); memcpy(l.m, x + 16, 64);
    if (axis) { *axis = -1; *sign = 1; snprintf(k, sizeof k, "A%08X", key); char av[8]; GetPrivateProfileStringA("Holds", k, "", av, sizeof av, g_ini);
        if ((av[0] == '+' || av[0] == '-') && av[1] >= 'X' && av[1] <= 'Z') { *sign = av[0] == '+' ? 1.0f : -1.0f; *axis = av[1] - 'X'; } }
    return true;
}
static void SaveHold(uint32_t key, const M4& r, const M4& l, int axis, float sign) {
    if (!g_ini[0]) return;
    char k[16], v[1024]; int o = 0; snprintf(k, sizeof k, "H%08X", key);
    const float* a = &r.m[0][0]; const float* b = &l.m[0][0];
    for (int i = 0; i < 32 && o < (int)sizeof v - 20; ++i) o += snprintf(v + o, sizeof v - o, "%s%.5f", i ? " " : "", i < 16 ? a[i] : b[i - 16]);
    WritePrivateProfileStringA("Holds", k, v, g_ini);
    if (axis >= 0) { char av[8]; snprintf(k, sizeof k, "A%08X", key); snprintf(av, sizeof av, "%c%c", sign > 0 ? '+' : '-', "XYZ"[axis]); WritePrivateProfileStringA("Holds", k, av, g_ini); }
}
 static void* volatile g_curWeapon = nullptr;
static bool ShotCurrent(const fc2vr::authority::Stamp& source, void* weapon) {
    return fc2vr::authority::SameOwner(weapon,g_curWeapon) && xr::InputCurrent(source,1);
}
static bool ReadAim(float o[3], float d[3], float* cameraForward = nullptr) {
    fc2vr::authority::Stamp shotSource,scopeSource; void* shotWeapon; void* scopeWeapon;
    float scopeO[3],scopeD[3]; bool valid,scope;
    Lock(); valid = g_shotValid; shotSource = g_shotSource; shotWeapon = g_shotWeapon;
    memcpy(o,g_shotO,12); memcpy(d,g_shotF,12); if (cameraForward) memcpy(cameraForward,g_camFwdNow,12);
    scope = g_scopeTick && GetTickCount()-g_scopeTick < 200;
    scopeSource = g_scopeSource; scopeWeapon = g_scopeWeapon; memcpy(scopeO,g_scopeO,12); memcpy(scopeD,g_scopeD,12); Unlock();
    if (!valid || !ShotCurrent(shotSource,shotWeapon)) return false;
    if (scope && scopeWeapon == shotWeapon && ShotCurrent(scopeSource,scopeWeapon)) { memcpy(o,scopeO,12); memcpy(d,scopeD,12); }
    return true;
}
// ---- diagnostic (phone calls and other special controls): which skeletons are animated, does the arm code run
// small extra skeletons (phone, map, watch models...): log when one appears or goes away, always (a few lines)
struct SmallSkel { void* p; int n; ULONGLONG seen; bool live; };
static SmallSkel g_small[12];
static void SmallSkelTick(uint8_t* skel) {
    if (!skel || skel == g_playerSkel || IsBadReadPtr(skel + 0x2C, 4)) return;
    int n = *(int*)(skel + 0x2C); if (n <= 0 || n >= 40) return;
    ULONGLONG t = GetTickCount64();
    for (auto& s : g_small) if (s.p == skel) { if (!s.live) { s.live = true; LOG("skeleton %p (%d bones) appeared (weapon %p, phone %d, normal controls %d)", skel, n, g_curWeapon, controls::PhoneOut(), controls::NormalControls()); } s.seen = t; return; }
    for (auto& s : g_small) if (!s.p || (!s.live && t - s.seen > 60000)) { s = { skel, n, t, true }; LOG("skeleton %p (%d bones) appeared (weapon %p, phone %d, normal controls %d)", skel, n, g_curWeapon, controls::PhoneOut(), controls::NormalControls()); return; }
}
static void SmallSkelSweep() {
    static ULONGLONG last = 0; ULONGLONG t = GetTickCount64(); if (t - last < 500) return; last = t;
    for (auto& s : g_small) if (s.p && s.live && t - s.seen > 500) { s.live = false; LOG("skeleton %p (%d bones) gone", s.p, s.n); }
}
static void DiagSkel(uint8_t* skel) {
    SmallSkelTick(skel);
    InterlockedIncrement(&g_pd.calls);
    if (skel == g_playerSkel) { InterlockedIncrement(&g_pd.player); return; }
    for (int i = 0; i < 4; ++i) {
        if (g_pd.other[i] == skel) { InterlockedIncrement(&g_pd.otherCalls[i]); return; }
        if (!g_pd.other[i]) { g_pd.other[i] = skel; g_pd.otherN[i] = skel && !IsBadReadPtr(skel + 0x2C, 4) ? *(int*)(skel + 0x2C) : -1; g_pd.otherCalls[i] = 1; return; }
    }
}
static void PhoneDiagReport() {
    static ULONGLONG last = 0; static bool wasSpecial = false; ULONGLONG t = GetTickCount64();
    bool special = !controls::NormalControls();
    if (special != wasSpecial) { wasSpecial = special; LOG("phone diag: special controls %s", special ? "ON (phone / map / cut scene)" : "off"); g_pd = PhoneDiag{}; last = t; return; }
    if (!special || t - last < 1000) return;
    last = t;
    LOG("phone diag 1s: skeleton updates %ld (player %ld, arms applied %ld, no frame %ld, no hand %ld), camera/hand updates %ld, player skel %p",
        g_pd.calls, g_pd.player, g_pd.applied, g_pd.noFrame, g_pd.noHand, g_pd.camUpd, g_playerSkel);
    for (int i = 0; i < 4; ++i) if (g_pd.other[i]) LOG("phone diag:   other skeleton %p (%d bones) updated %ld times", g_pd.other[i], g_pd.otherN[i], g_pd.otherCalls[i]);
    g_pd = PhoneDiag{};
}
static volatile bool g_reloadOn = false; static volatile LONG g_reloadClip = -1; static volatile DWORD g_reloadEndAt = 0;
static uint8_t* volatile g_playerEntR = nullptr;   // the player entity (for telling the player's reloads apart)
static volatile int g_playerAnimId = -1;           // learned: the animation id the player's reloads are sent with
static void ReloadBegin(const char* how) {
    if (!g_c.freezeReload) return;
    g_reloadOn = true; g_reloadT = GetTickCount(); g_reloadEndAt = 0;
    static int n = 0; if (n++ < 30) LOG("arms: reload started (%s, weapon %p) - arms held still", how, g_curWeapon);
}
static bool ReloadActive() {
    if (!g_reloadOn) return false;
    DWORD now = GetTickCount(), dt = now - g_reloadT;
    void* st = g_reloadState;
    if (st && !IsBadReadPtr((uint8_t*)st + 5, 1) && ((uint8_t*)st)[5] != 0 && !g_reloadEndAt) g_reloadEndAt = now + 150;   // reload_done
    if ((g_reloadEndAt && now >= g_reloadEndAt) || dt > 7000) {
        g_reloadOn = false; g_reloadState = nullptr;
        LOG("arms: reload over (%s, %.1f s)", dt > 7000 ? "time out" : "done", dt / 1000.0f);
        return false;
    }
    return true;
}
// The player's reloads, from the HUD's ammo numbers (the reload animation itself isn't sent through the generic
// animation event path for the player): a reload starts with the reload button while the clip isn't full, or when
// the clip runs empty with ammo left (automatic reload); it ends once the clip has been refilled (a moment after the
// last rise - shotguns load shell by shell), on a weapon change, or after 7 s.
static volatile LONG g_clip = -1, g_pouch = -1; static int g_clipMax[64]; static uint32_t g_clipKey[64]; static int g_nClipKey = 0;
static int& ClipMax(uint32_t key) { for (int i = 0; i < g_nClipKey; ++i) if (g_clipKey[i] == key) return g_clipMax[i]; int i = g_nClipKey < 64 ? g_nClipKey++ : 63; g_clipKey[i] = key; g_clipMax[i] = 0; return g_clipMax[i]; }
void NotifyClip(int clip, int pouch) {
    int last = g_clip;
    { static int nl = 0; if (nl++ < 40) LOG("arms: HUD ammo %d / %d (weapon %p)", clip, pouch, g_curWeapon); }
    g_clip = clip; g_pouch = pouch;
    if (!g_curWeapon) return;
    int& mx = ClipMax(g_weaponKey);
    if (clip > mx) mx = clip;
    if (g_reloadOn && last >= 0 && clip > last) g_reloadEndAt = GetTickCount() + (clip >= mx ? 250 : 700);
    else if (!g_reloadOn && last > 0 && clip == 0 && pouch > 0) ReloadBegin("clip empty");
}
static void ReloadInputTick() {   // game thread, each frame
    static void* lastW = nullptr;
    if (g_curWeapon != lastW) { lastW = g_curWeapon; if (g_reloadOn) { g_reloadOn = false; g_reloadState = nullptr; LOG("arms: reload over (weapon changed)"); } }
    if (controls::TakeReloadPress() && g_curWeapon && !g_reloadOn) {
        int c = g_clip, p = g_pouch, mx = ClipMax(g_weaponKey);
        if (c >= 0 && p > 0 && c < mx) ReloadBegin("reload button");
    }
}
// Every animation event goes through 0x101FA340 (thiscall anim system, int animId, const uint32_t* eventHash, params;
// ret 0xC). The "reload" event (hash at 0x10FE4DA4) is sent for every reloading character; the player's is told apart
// by its animation id (compared with the ids the player's pawn / entity / weapon carry at +0x150 / +0x158).
typedef void (__fastcall *AnimEvt_t)(void* self, void* edx, int id, const uint32_t* hash, void* params);
static AnimEvt_t o_AnimEvt = nullptr;
static void __fastcall Hook_AnimEvt(void* self, void* edx, int id, const uint32_t* hash, void* params) {
    if (hash == (const uint32_t*)A(0x10FE4DA4) && id != -1) {
        bool player = id == g_playerAnimId;
        const char* how = player ? "learned id" : nullptr;
        uint8_t* objs[4] = { g_pawn, g_pawn && !IsBadReadPtr(g_pawn + 0x10, 4) ? *(uint8_t**)(g_pawn + 0x10) : nullptr, g_playerEntR, (uint8_t*)g_curWeapon };
        static const char* on[4] = { "pawn", "pawn data", "entity", "weapon" };
        char cand[200]; int o = 0; cand[0] = 0;
        for (int i = 0; i < 4 && !player; ++i) {
            uint8_t* q = objs[i]; if (!q || IsBadReadPtr(q + 0x150, 12)) continue;
            for (int k = 0; k < 3; ++k) { int v = *(int*)(q + 0x150 + k * 4); if (v == id) { player = true; how = on[i]; g_playerAnimId = id; } if (o < 180) o += snprintf(cand + o, sizeof cand - o, " %s+%x=%d", on[i], 0x150 + k * 4, v); }
        }
        static int nl = 0; if (nl++ < 12) LOG("arms: reload event id %d -> %s (%s)", id, player ? "player" : "other", player ? how : cand);
        if (player) ReloadBegin("reload event");
    }
    o_AnimEvt(self, edx, id, hash, params);
}
extern "C" void __cdecl ReloadStartHook(uint8_t* state) {
    if (!state || IsBadReadPtr(state + 0xC, 4)) return;
    uint8_t* owner = *(uint8_t**)(state + 0xC);
    if (!owner || IsBadReadPtr(owner, 0x6C)) return;
    typedef uint8_t* (__fastcall *Pawn_t)(void* owner);
    uint8_t* pawn = ((Pawn_t)A(0x100EA6C0))(owner);
    if (!pawn) return;
    uint8_t* pdat = !IsBadReadPtr(pawn + 0x10, 4) ? *(uint8_t**)(pawn + 0x10) : nullptr;
    bool player = pawn == g_pawn || (pdat && !IsBadReadPtr(pdat + 0x499, 1) && pdat[0x499]);
    if (!player) return;
    g_reloadState = state; ReloadBegin("reload state");
}
static bool InstallReloadDetour() {
    uint8_t* site = (uint8_t*)A(0x10166501);
    static const uint8_t orig[9] = { 0x8B, 0x57, 0x0C, 0x8B, 0x82, 0x50, 0x01, 0x00, 0x00 };   // mov edx,[edi+0Ch]; mov eax,[edx+150h]
    if (memcmp(site, orig, 9) != 0) { LOG("arms: reload detour - unexpected code, not installed"); return false; }
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    uint8_t* q = stub;
    *q++ = 0x60;                                   // pushad
    *q++ = 0x9C;                                   // pushfd
    *q++ = 0x57;                                   // push edi
    *q++ = 0xE8; int32_t rel = (int32_t)((uint8_t*)&ReloadStartHook - (q + 4)); memcpy(q, &rel, 4); q += 4;   // call
    *q++ = 0x83; *q++ = 0xC4; *q++ = 0x04;         // add esp,4
    *q++ = 0x9D;                                   // popfd
    *q++ = 0x61;                                   // popad
    memcpy(q, orig, 9); q += 9;                    // the original two instructions
    *q++ = 0xE9; rel = (int32_t)((site + 9) - (q + 4)); memcpy(q, &rel, 4); q += 4;   // jmp back
    DWORD old;
    if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9; rel = (int32_t)(stub - (site + 5)); memcpy(site + 1, &rel, 4);
    for (int i = 5; i < 9; ++i) site[i] = 0x90;
    VirtualProtect(site, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 9);
    return true;
}
// The weapon models' own small skeletons (the player's first-person weapon and its parts): their root bone carries the
// weapon's reload motion (the crossbow rolled onto its side to load a bolt, magazines pulled out). With FreezeReload
// the root is held at its rest pose (taken once the skeleton has been still for a moment after appearing); the
// other bones keep their motion relative to it.
[[maybe_unused]] static void WeaponSkelLock(uint8_t* skel) {
    if (!g_c.freezeReload || IsBadReadPtr(skel + 0x2C, 4)) return;
    int si = -1; for (int i = 0; i < 12; ++i) if (g_small[i].p == skel && g_small[i].live) { si = i; break; }
    if (si < 0) return;
    Bone* bones = *(Bone**)(skel + 0x20); int n = *(int*)(skel + 0x2C);
    if (!bones || n <= 0 || n >= 40 || IsBadReadPtr(bones, n * sizeof(Bone))) return;
    struct Rest { void* skel; M4 rest, prev; bool have; ULONGLONG born, still; void* w; M4 b0[40]; float mdeg[40], mcm[40]; bool win; };
    static Rest rs[12];
    Rest& r = rs[si];
    ULONGLONG t = GetTickCount64();
    M4 now; memcpy(now.m, bones[0].model, 64); now.m[0][3] = now.m[1][3] = now.m[2][3] = 0; now.m[3][3] = 1;
    if (r.skel != skel || r.w != g_curWeapon) { r.skel = skel; r.w = g_curWeapon; r.have = false; r.born = t; r.still = t; r.prev = now; }
    if (!r.have) {
        float dp = 0, dr = 0; for (int k = 0; k < 3; ++k) { float d = now.m[3][k] - r.prev.m[3][k]; dp += d * d; dr += Dot3(now.m[k], r.prev.m[k]); }
        if (!(sqrtf(dp) < 0.002f && dr > 2.998f) || !controls::NormalControls() || ReloadActive()) r.still = t;
        r.prev = now;
        if (t - r.born > 1200 && t - r.still > 400) { r.rest = now; r.have = true; LOG("arms: weapon model skeleton %p (%d bones) root held at rest", skel, n); }
        return;
    }
    float dr = 0; for (int k = 0; k < 3; ++k) dr += Dot3(now.m[k], r.rest.m[k]);
    float dp = 0; for (int k = 0; k < 3; ++k) { float d = now.m[3][k] - r.rest.m[3][k]; dp += d * d; }
    {   // diagnostic window (3.5 s after a projectile shot): how far each bone of this model moves from where it was
        bool win = t < g_diagUntil;
        if (win && !r.win) for (int i = 0; i < n; ++i) { memcpy(r.b0[i].m, bones[i].model, 64); r.mdeg[i] = r.mcm[i] = 0; }
        if (win) for (int i = 0; i < n; ++i) {
            const float* m = bones[i].model; float tr = 0, d2 = 0;
            for (int k = 0; k < 3; ++k) { float a[3] = { m[k * 4], m[k * 4 + 1], m[k * 4 + 2] }, b[3] = { r.b0[i].m[k][0], r.b0[i].m[k][1], r.b0[i].m[k][2] };
                float la = sqrtf(Dot3(a, a)), lb = sqrtf(Dot3(b, b)); if (la > 1e-6f && lb > 1e-6f) tr += Dot3(a, b) / (la * lb);
                float d = m[12 + k] - r.b0[i].m[3][k]; d2 += d * d; }
            float ang = acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
            if (ang > r.mdeg[i]) r.mdeg[i] = ang; if (sqrtf(d2) * 100.0f > r.mcm[i]) r.mcm[i] = sqrtf(d2) * 100.0f;
        }
        if (!win && r.win) {
            char b[400]; int o = snprintf(b, sizeof b, "arms: model skeleton %p (%d bones) moved in the shot window:", skel, n);
            for (int i = 0; i < n && o < 380; ++i) o += snprintf(b + o, sizeof b - o, " %d:%.0fdeg/%.0fcm", i, r.mdeg[i], r.mcm[i]);
            LOG("%s", b);
        }
        r.win = win;
    }
    if (dr > 2.9999f && dp < 1e-8f) return;   // at rest already
    { static ULONGLONG lastL = 0; if (t - lastL > 2000) { lastL = t; LOG("arms: weapon model skeleton %p root moved %.0f deg / %.2f m - held", skel, acosf(fmaxf(-1.0f, fminf(1.0f, (dr - 1.0f) * 0.5f))) * 57.2958f, sqrtf(dp)); } }
    M4 inow; if (!Inv(now, inow)) return;
    M4 corr = Mul(inow, r.rest);
    for (int i = 0; i < n; ++i) {
        M4 b; memcpy(b.m, bones[i].model, 64);
        float w3[4] = { b.m[0][3], b.m[1][3], b.m[2][3], b.m[3][3] };
        b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1;
        M4 o = Mul(b, corr);
        o.m[0][3] = w3[0]; o.m[1][3] = w3[1]; o.m[2][3] = w3[2]; o.m[3][3] = w3[3];
        memcpy(bones[i].model, o.m, 64);
    }
}
// The held weapon's model: its graphic component's world matrix (+0x170) relative to the final right hand. With
// FreezeReload it is held where it sat in the hand once settled (the crossbow's bolt reload turned the whole model
// away from the hand); during 3.5 s after a projectile shot its motion is logged.
static void CallerReport();
static void AttachWatch();
// ---- v213 monocular: the map strategy (CGadgetMapStrategy) equips it in 0x106EFB40 (thiscall, no args): hides the map
// and GPS, attaches the monocular entity (handle at +0x10C, entity at handle+0xC) to the player; +0x2D9 = equipped.
// The game hangs it on a bone the mod doesn't move (it floated in front of the camera); it is re-hung on the right
// hand with the offset it had relative to the game's animated right hand (which is shaped around it).
static void* volatile g_mapStrat = nullptr;
static uint8_t* volatile g_monoAt = nullptr; static volatile int g_monoBone = -1; static float g_monoOff[16];
static volatile ULONGLONG g_monoSince = 0; static M4 g_monoRel; static volatile bool g_monoRelValid = false;
static M4 g_monoG, g_monoP; static volatile DWORD g_monoCamTick = 0;
static uint8_t* volatile g_monoGfx = nullptr;
// v216 tagging: the binoculars update (0x1072C260, thiscall) casts its tag ray from the camera bone's position
// (0x1010D780: thiscall(aim, slot, float3* out)) along its rotation (0x1010D850: thiscall(aim, slot, quat* out)) applied to
// +Y. While the monocular is at the eye that ray comes from the head-aimed scope camera instead.
static volatile bool g_inMonoUpd = false;
typedef void (__fastcall *MonoUpd_t)(void* self, void* edx);
static MonoUpd_t o_MonoUpd = nullptr;
// v217: the binoculars strategy keeps the taggable thing under its ray at +0x118 (a handle; entity at +0xC, null when
// nothing taggable is in the ring) - the mod's own targeting ring turns green from it
static volatile bool g_monoTarget = false; static volatile DWORD g_monoTargetTick = 0;
static void __fastcall Hook_MonoUpd(void* self, void* edx) {
    g_inMonoUpd = true; o_MonoUpd(self, edx); g_inMonoUpd = false;
    uint8_t* st = (uint8_t*)self; bool t = false;
    if (st && !IsBadReadPtr(st + 0x118, 4)) { uint8_t* h = *(uint8_t**)(st + 0x118); t = h && !IsBadReadPtr(h + 0xC, 4) && *(void**)(h + 0xC) != nullptr; }
    if (t != g_monoTarget) { g_monoTarget = t; static int nl = 0; if (nl++ < 20) LOG("monocular: %s", t ? "taggable target in the ring" : "no target in the ring"); }
    g_monoTargetTick = GetTickCount();
}
static bool MonoAimRay(float o[3], float d[3]);
typedef void (__fastcall *AimPos_t)(void* self, void* edx, int slot, float* out);
static AimPos_t o_AimPos = nullptr;
static void __fastcall Hook_AimPos(void* self, void* edx, int slot, float* out) {
    o_AimPos(self, edx, slot, out);
    float o[3], d[3]; if (slot == 0 && out && MonoAimRay(o, d)) memcpy(out, o, 12);
}
typedef void (__fastcall *AimRot_t)(void* self, void* edx, int slot, float* out);
static AimRot_t o_AimRot = nullptr;
static void __fastcall Hook_AimRot(void* self, void* edx, int slot, float* out) {
    o_AimRot(self, edx, slot, out);
    float o[3], d[3];
    if (slot != 0 || !out || !MonoAimRay(o, d)) return;
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (l < 1e-6f) return;
    for (float& x : d) x /= l;
    alignas(16) float q[4] = { d[2], 0.0f, -d[0], 1.0f + d[1] };   // shortest turn from +Y to d
    if (q[3] < 1e-4f) { q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0; }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); for (float& x : q) x /= n;
    static int conv = 0;   // 0 unknown, 1 as built, -1 conjugate (checked with the game's own rotation)
    if (!conv) {
        typedef float* (__fastcall *Rot_t)(const float* q, void* edx, float* out, const float* v);
        alignas(16) float v[4] = { 0, 1, 0, 0 }, r[4] = {};
        ((Rot_t)A(0x10032EE0))(q, nullptr, r, v);
        conv = (r[0] * d[0] + r[1] * d[1] + r[2] * d[2]) > 0.9f ? 1 : -1;
        LOG("monocular: tag ray rotation %s (check %.2f)", conv > 0 ? "as built" : "conjugated", r[0] * d[0] + r[1] * d[1] + r[2] * d[2]);
    }
    if (conv < 0) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; }
    memcpy(out, q, 16);
    static int nl = 0; if (nl++ < 3) LOG("monocular: tag ray from the head (dir %.2f %.2f %.2f)", d[0], d[1], d[2]);
}   // v215: the game's render camera and the scope camera posed from the head
typedef void (__fastcall *EquipMono_t)(void* self, void* edx);
static EquipMono_t o_EquipMono = nullptr;
static void __fastcall Hook_EquipMono(void* self, void* edx) { o_EquipMono(self, edx); g_mapStrat = self; LOG("monocular: equipped (map strategy %p)", self); }
// v218: the map's and GPS's marker icons (player arrow, objective rings, supply icons - new ones too after a tag) are
// drawn with the map and the GPS, which the game only hides (and lowers out of its flat view) while the monocular is out.
// In VR they stayed where the hands hold them. While the monocular is out the map and GPS entities are moved far away
// (their attachment offset, +0x78 -> +0x80, translation +500 m) and put back when it's away. Map: map strategy +0x108
// handle; GPS: [[strategy+0x38]+8]+0xC. (v217 hid entities from the strategy's marker lists - those were world objects.)
struct MovedAt { uint8_t* at; float base[3]; float wrote[3]; };
static MovedAt g_moved[2]; static int g_movedN = 0;
static uint8_t* EntOfHandle(uint8_t* h) { return (h && !IsBadReadPtr(h, 0x10)) ? *(uint8_t**)(h + 0xC) : nullptr; }
static void MonoIconsHide(uint8_t* st, uint8_t* mono) {
    uint8_t* ents[2] = { nullptr, nullptr };
    if (!IsBadReadPtr(st + 0x108, 4)) ents[0] = EntOfHandle(*(uint8_t**)(st + 0x108));
    if (!IsBadReadPtr(st + 0x38, 4)) { uint8_t* o = *(uint8_t**)(st + 0x38); if (o && !IsBadReadPtr(o + 8, 4)) ents[1] = EntOfHandle(*(uint8_t**)(o + 8)); }
    for (int k = 0; k < 2; ++k) {
        uint8_t* e = ents[k];
        if (!e || e == mono || IsBadReadPtr(e + 0x78, 4)) continue;
        uint8_t* at = *(uint8_t**)(e + 0x78);
        if (!at || IsBadWritePtr(at + 0x80, 64)) continue;
        float* t = (float*)(at + 0x80) + 12;
        MovedAt* m = nullptr; for (int i = 0; i < g_movedN; ++i) if (g_moved[i].at == at) m = &g_moved[i];
        if (!m) { if (g_movedN >= 2) continue; m = &g_moved[g_movedN++]; m->at = at; memcpy(m->base, t, 12);
            LOG("monocular: %s moved away (entity %p, attachment %p on bone %d)", k ? "GPS" : "map", e, at, *(int*)(at + 0x1C)); }
        else if (memcmp(t, m->wrote, 12)) memcpy(m->base, t, 12);   // the game placed it again
        for (int j = 0; j < 3; ++j) m->wrote[j] = m->base[j] + 500.0f;
        memcpy(t, m->wrote, 12);
    }
}
static void MonoIconsShow() {
    if (!g_movedN) return;
    for (int i = 0; i < g_movedN; ++i) {
        MovedAt& m = g_moved[i];
        if (IsBadWritePtr(m.at + 0x80, 64)) continue;
        float* t = (float*)(m.at + 0x80) + 12;
        if (!memcmp(t, m.wrote, 12)) memcpy(t, m.base, 12);
    }
    LOG("monocular: map / GPS put back (%d)", g_movedN); g_movedN = 0;
}
typedef void (__fastcall *SetVis_t)(void* ent, void* edx, int on);

// v220 flashlight: a spot light from the game's light manager (as the vehicle headlights: 0x101AE6C0) placed at the
// head and aimed where it looks. Manager object 0x10F96830: Alloc 0x10459440 thiscall(mgr, uint32 handle[2]*, 0),
// Get 0x104596F0 thiscall(mgr, handle*, 1) -> light (0x7C bytes), Free 0x10459550 thiscall(mgr, h0, h1).
// Light: +4 type (3 = spot), +8 on, +0xC position, +0x18 direction, +0x24 up, +0x30 range, +0x34 colour RGB,
// +0x5C outer angle, +0x60 inner angle (radians). Freed in menus / loading screens (re-made after).
static float g_headW[9]; static volatile DWORD g_headWTick = 0;
void SetHeadWorld(const float pos[3], const float dir[3], const float up[3]) {
    if (!g_csInit) return; Lock(); memcpy(g_headW, pos, 12); memcpy(g_headW + 3, dir, 12); memcpy(g_headW + 6, up, 12); g_headWTick = GetTickCount(); Unlock();
}
static uint32_t g_flH[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static void FlashTick() {   // game thread
    typedef void (__fastcall *LAlloc_t)(void* mgr, void* edx, uint32_t* out, int z);
    typedef uint8_t* (__fastcall *LGet_t)(void* mgr, void* edx, uint32_t* h, int w);
    typedef void (__fastcall *LFree_t)(void* mgr, void* edx, uint32_t h0, uint32_t h1);
    void* mgr = (void*)A(0x10F96830);
    if (!mgr || !A(0x10459440) || !A(0x104596F0) || !A(0x10459550)) return;
    bool menu = controls::MenuControls() || controls::LoadingControls();
    bool fresh = g_csInit && GetTickCount() - g_headWTick < 250;
    bool want = controls::FlashlightOn() && !menu && fresh;
    if (g_flH[0] != 0xFFFFFFFFu && (menu || !controls::FlashlightOn())) {   // off: give the light back
        ((LFree_t)A(0x10459550))(mgr, nullptr, g_flH[0], g_flH[1]); g_flH[0] = g_flH[1] = 0xFFFFFFFFu;
        LOG("flashlight: light freed%s", menu ? " (menu / loading)" : ""); return;
    }
    if (!want) return;
    if (g_flH[0] == 0xFFFFFFFFu) { uint32_t h[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu }; ((LAlloc_t)A(0x10459440))(mgr, nullptr, h, 0);
        if (h[0] == 0xFFFFFFFFu) return; g_flH[0] = h[0]; g_flH[1] = h[1]; LOG("flashlight: light %08X %08X made", h[0], h[1]); }
    uint8_t* L = ((LGet_t)A(0x104596F0))(mgr, nullptr, g_flH, 1);
    if (!L || IsBadWritePtr(L, 0x7C)) return;
    float hw[9]; Lock(); memcpy(hw, g_headW, 36); Unlock();
    const float k = 0.0174532924f, b = g_c.flashBright;
    *(int*)(L + 4) = 3; L[8] = 1;
    memcpy(L + 0xC, hw, 12); memcpy(L + 0x18, hw + 3, 12); memcpy(L + 0x24, hw + 6, 12);
    *(float*)(L + 0x30) = g_c.flashRange;
    *(float*)(L + 0x34) = 1.0f * b; *(float*)(L + 0x38) = 0.96f * b; *(float*)(L + 0x3C) = 0.86f * b;
    *(float*)(L + 0x5C) = g_c.flashOuter * k; *(float*)(L + 0x60) = g_c.flashInner * k;
}
static uint8_t* g_monoHidEnt = nullptr;   // the monocular model, hidden while looking through it (the mod draws the ring)
static void MonoWatch() {   // game thread, each frame
    uint8_t* st = (uint8_t*)g_mapStrat; int hand = g_slot[12];
    uint8_t* ent = nullptr;
    if (st && !IsBadReadPtr(st + 0x10C, 0x1D0) && st[0x2D9]) {
        uint8_t* h = *(uint8_t**)(st + 0x10C);
        if (h && !IsBadReadPtr(h + 0xC, 4)) ent = *(uint8_t**)(h + 0xC);
    }
    if (ent) MonoIconsHide(st, ent); else MonoIconsShow();
    if (g_monoHidEnt && (g_monoHidEnt != ent || !controls::MonocularAtFace())) {
        if (g_monoHidEnt == ent) ((SetVis_t)A(0x104DDB20))(ent, nullptr, 1);
        g_monoHidEnt = nullptr;
    }
    uint8_t* at = (ent && !IsBadReadPtr(ent + 0x78, 4)) ? *(uint8_t**)(ent + 0x78) : nullptr;
    if (!at || IsBadReadPtr(at, 0xC0) || hand < 0) {
        if (g_monoAt) LOG("monocular: put away");
        g_monoAt = nullptr; g_monoBone = -1; g_monoRelValid = false; g_monoSince = 0; return;
    }
    int idx = *(int*)(at + 0x1C); float* off = (float*)(at + 0x80);
    if (at != g_monoAt) { g_monoAt = at; g_monoSince = GetTickCount64(); g_monoRelValid = false; g_monoBone = -1;
        LOG("monocular: entity %p attachment %p on bone %d (right hand %d), offset %.2f %.2f %.2f", ent, at, idx, hand, off[12], off[13], off[14]); }
    if (idx != hand && !g_monoRelValid) { g_monoBone = idx; memcpy(g_monoOff, off, 64); }   // the game's own placement (Apply works out the hand offset)
    // v216: at the eye it hangs where the game puts it (on the camera bone, its lens and red targeting ring centred on the
    // game's view); MonoRenderFix moves the drawn model with the head-aimed scope camera right before that view is drawn
    {   typedef uint8_t* (__fastcall *Comp_t)(void* ent, void* edx, const void* type);
        uint8_t* gfx = *(uint32_t*)A(0x10FDF194) ? ((Comp_t)A(0x104E2CC0))(ent, nullptr, (const void*)A(0x10FDF1AC)) : nullptr;
        g_monoGfx = (gfx && !IsBadReadPtr(gfx + 0x170, 64)) ? gfx : nullptr; }
    if (controls::MonocularAtFace() && g_monoBone >= 0) {
        float r[16]; memcpy(r, g_monoOff, 64);
        *(int*)(at + 0x1C) = g_monoBone; memcpy(off, r, 64);
        if (!g_monoHidEnt && !IsBadReadPtr(ent + 0x70, 4)) { ((SetVis_t)A(0x104DDB20))(ent, nullptr, 0); g_monoHidEnt = ent; }   // v217
        return;
    }
    if (g_monoRelValid) {
        float r[16]; memcpy(r, g_monoRel.m, 64); r[3] = r[7] = r[11] = 0; r[15] = 1;
        if (idx != hand || memcmp(off, r, 64)) { *(int*)(at + 0x1C) = hand; memcpy(off, r, 64);
            static int nl = 0; if (nl++ < 3) LOG("monocular: hung on the right hand"); }
    }
}

static void HeldWeaponGfx(uint8_t* skel) {
    uint8_t* gfx = g_wGfx;
    if (!gfx || !g_havePrevFinal || IsBadReadPtr(gfx + 0x170, 64)) return;
    M4 gw; memcpy(gw.m, gfx + 0x170, 64);
    float w3[4] = { gw.m[0][3], gw.m[1][3], gw.m[2][3], gw.m[3][3] };
    gw.m[0][3] = gw.m[1][3] = gw.m[2][3] = 0; gw.m[3][3] = 1;
    M4 ih; if (!Inv(g_prevFinalRh, ih)) return;
    M4 rel = Mul(gw, ih);
    static M4 rest, prev; static bool have = false; static void* restW = nullptr; static ULONGLONG born = 0, still = 0;
    static float restBones[64][16]; static int restN = 0;
    static bool win = false; static float mx = 0, mxRot = 0; static int frames = 0;
    ULONGLONG t = GetTickCount64();
    auto dev = [](const M4& a, const M4& b, float* rot) { float tr = 0; for (int k = 0; k < 3; ++k) tr += Dot3(a.m[k], b.m[k]);
        float ang = acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f; float d[3] = { a.m[3][0] - b.m[3][0], a.m[3][1] - b.m[3][1], a.m[3][2] - b.m[3][2] };
        if (rot) *rot = ang; return ang + sqrtf(Dot3(d, d)) * 100.0f; };
    if (restW != g_curWeapon) { restW = g_curWeapon; have = false; born = t; still = t; prev = rel; }
    if (!have) {
        if (dev(rel, prev, nullptr) > 0.5f || !controls::NormalControls() || ReloadActive() || t < g_diagUntil + 800) still = t;   // never inside a reload (the crossbow's runs ~3 s after each shot)
        prev = rel;
        if (t - born > 1500 && t - still > 600) {
            rest = rel; have = true; restN = 0;
            uint8_t* sk = g_wSkel;
            if (sk && !IsBadReadPtr(sk + 0x2C, 4)) { int n = *(int*)(sk + 0x2C); Bone* b = *(Bone**)(sk + 0x20);
                if (b && n > 0 && n <= 64 && !IsBadReadPtr(b, n * sizeof(Bone))) { restN = n; for (int i = 0; i < n; ++i) memcpy(restBones[i], b[i].model, 64); } }
            LOG("arms: held weapon model %p held in the hand (rest taken, %d bones)", gfx, restN);
        }
    }
    bool w = t < g_diagUntil;
    if (w && !win) { mx = mxRot = 0; frames = 0; }
    if (have) { float r; float d = dev(rel, rest, &r); if (w) { if (d > mx) mx = d; if (r > mxRot) mxRot = r; ++frames; } }
    static M4 lastWritten; static bool wrote = false; static int kept = 0, lost = 0, changedN = 0; static M4 lastSeen;
    if (wrote) { if (memcmp(gfx + 0x170, lastWritten.m, 64) == 0) ++kept; else ++lost; }
    if (memcmp(gfx + 0x170, lastSeen.m, 64) != 0) ++changedN; memcpy(lastSeen.m, gfx + 0x170, 64);
    if (w && !win) { kept = lost = changedN = 0; }
    if (!w && win) LOG("arms: held weapon model moved up to %.1f (deg+cm), %.0f deg, from its rest in the hand over %d frames after the shot; matrix changed in %d frames, our hold kept %d / overwritten %d", mx, mxRot, frames, changedN, kept, lost);
    win = w;
    if (have && g_c.freezeReload && false && (w || dev(rel, rest, nullptr) > 3.0f)) {   // off: the attachment is pinned at its source instead (AttachWatch)   // after a projectile shot: always (the game writes it only now and then)   // only when the game has moved it out of the hand (normal holding untouched)
        M4 o = Mul(rest, g_prevFinalRh);
        o.m[0][3] = w3[0]; o.m[1][3] = w3[1]; o.m[2][3] = w3[2]; o.m[3][3] = w3[3];
        memcpy(gfx + 0x170, o.m, 64); memcpy(lastWritten.m, o.m, 64); wrote = true;
        // the model's own bones too: during the bolt reload the game moves the model far away and brings the mesh
        // back with its bones - with only the model held in the hand, the bones carried the mesh out of view
        uint8_t* sk = g_wSkel;
        if (w && restN > 0 && sk && !IsBadReadPtr(sk + 0x2C, 4) && *(int*)(sk + 0x2C) == restN) {
            Bone* b = *(Bone**)(sk + 0x20);
            if (b && !IsBadWritePtr(b, restN * sizeof(Bone))) for (int i = 0; i < restN; ++i) memcpy(b[i].model, restBones[i], 64);
        }
    } else wrote = false;
}
static void Apply(uint8_t* skel) {
    DiagSkel(skel); if (skel == g_playerSkel) SmallSkelSweep(); if (g_c.phoneDiag) PhoneDiagReport();
    if (skel && skel != g_playerSkel) { if (skel == g_wSkel) { static int n = 0; if (n++ < 3) LOG("arms: held weapon skeleton %p updated through the hierarchy hook", skel); } return; }   // v199: WeaponSkelLock removed - it froze NPC weapons (small skeletons) in their slung pose
    if (!skel) return;
    Bone* bones = *(Bone**)(skel + 0x20); int n = *(int*)(skel + 0x2C);
    if (!bones || n <= 0 || n > 512) return;
    int cam = g_slot[0], rh = g_slot[12];
    if (cam < 0 || cam >= n || rh < 0 || rh >= n) return;
    uint8_t* gfx = (uint8_t*)g_playerGfx; if (!gfx) return;
    if (skel != g_maskSkel || n != g_maskCount) BuildMask(skel, bones, n);
    // world matrix of the skeleton
    M4 W; memcpy(W.m, gfx + 0x170, 64); W.m[0][3] = W.m[1][3] = W.m[2][3] = 0; W.m[3][3] = 1;
    M4 Winv; if (!Inv(W, Winv)) return;
    // camera bone in world
    M4 camM; memcpy(camM.m, bones[cam].model, 64); camM.m[0][3] = camM.m[1][3] = camM.m[2][3] = 0; camM.m[3][3] = 1;
    M4 camW = Mul(camM, W);
    {
        Lock();
        for (int i = 0; i < 3; ++i) { for (int k = 0; k < 3; ++k) g_camBoneRot[i][k] = camW.m[i][k]; g_camBonePos[i] = camW.m[3][i]; }
        Unlock();
        InterlockedIncrement(&g_camBoneSeq);
    }
    if (!g_dumped && g_c.dump) { g_dumped = true; Dump(bones, n); }
    if (!g_c.weaponInHand || !g_permOk) return;
    Frame f; Lock(); f = g_frame; Unlock();
    if (!f.valid) { InterlockedIncrement(&g_pd.noFrame); return; }
    if (!f.handValid) { InterlockedIncrement(&g_pd.noHand); return; }
    {   // diagnostic: the hands as animated, relative to the camera bone (which one moves during a phone call)
        static ULONGLONG lastD = 0; ULONGLONG tn = GetTickCount64();
        if (g_c.phoneDiag && !controls::NormalControls() && tn - lastD > 700) {
            lastD = tn;
            int hs[2] = { g_slot[20], g_slot[12] }; M4 ic; float rel[2][3] = {};
            if (Inv(camW, ic)) for (int s = 0; s < 2; ++s) if (hs[s] >= 0 && hs[s] < n) {
                M4 b; memcpy(b.m, bones[hs[s]].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1;
                M4 r = Mul(Mul(b, W), ic); for (int k = 0; k < 3; ++k) rel[s][k] = r.m[3][k];
            }
            LOG("phone diag: animated hands rel. camera  L %.2f %.2f %.2f  R %.2f %.2f %.2f  (weapon %p)", rel[0][0], rel[0][1], rel[0][2], rel[1][0], rel[1][1], rel[1][2], g_curWeapon);
        }
    }
    // camera frame (right, up, back) from the bone's axes
    float cr[3], cf[3], cu[3], cb[3], co[3];
    for (int k = 0; k < 3; ++k) {
        cr[k] = g_perm[0][0] * camW.m[0][k] + g_perm[0][1] * camW.m[1][k] + g_perm[0][2] * camW.m[2][k];
        cf[k] = g_perm[1][0] * camW.m[0][k] + g_perm[1][1] * camW.m[1][k] + g_perm[1][2] * camW.m[2][k];
        cu[k] = g_perm[2][0] * camW.m[0][k] + g_perm[2][1] * camW.m[1][k] + g_perm[2][2] * camW.m[2][k];
        cb[k] = -cf[k]; co[k] = camW.m[3][k];
    }
    Normalize(cr); Normalize(cu); Normalize(cb); Normalize(cf);
    // Published effective origin/basis, not the current animated Camera bone.
    // camW still owns animation mapping/calibration, never the tracking origin.
    // v231 body clock: the frame was published from the camera one game frame earlier (the skeleton is updated
    // before the render thread publishes this frame's sample). Move the published origin by how far the Camera bone
    // has moved since, so the hands keep pace with walking, vehicles and the body (no lag-dependent jitter).
    if (g_c.bodyClock && f.bonePubValid) {
        float d[3] = { co[0] - f.bonePub[0], co[1] - f.bonePub[1], co[2] - f.bonePub[2] };
        float dl = sqrtf(Dot3(d, d));
        static double sum = 0; static int cnt = 0, nLog = 0; static ULONGLONG lastL = 0;
        if (std::isfinite(dl) && dl < 1.5f) { f.game.P.x += d[0]; f.game.P.y += d[1]; f.game.P.z += d[2]; sum += dl; ++cnt; }
        ULONGLONG tn = GetTickCount64();
        if (nLog < 30 && tn - lastL > 10000 && cnt) { ++nLog; lastL = tn; LOG("body clock: hands moved with the body by %.1f mm per frame on average (%d frames)", sum / cnt * 1000.0, cnt); sum = 0; cnt = 0; }
    }
    auto toWorld = [&](const float* ref, float* out, bool point) {
        Put3(out, point ? f.game.Position(Read3(ref)) : f.game.Direction(Read3(ref)));
    };
    auto toRef = [&](const float* w, float* out) {
        Put3(out, f.game.ReferencePosition(Read3(w)));
    };
    float ar[3], au[3], ab[3], ao[3], grip[3];
    toWorld(f.aimR, ar, false); toWorld(f.aimU, au, false); toWorld(f.aimB, ab, false); toWorld(f.aimO, ao, true); toWorld(f.gripO, grip, true);
    M4 C = FromRows(cr, cu, cb, co), Cinv; if (!Inv(C, Cinv)) return;
    M4 Am = FromRows(ar, au, ab, ao);
    M4 Map = Mul(Cinv, Am);
    // world matrices of the arm bones as animated
    static M4 Bw[512];
    static M4 Banim[512];
    for (int i = 0; i < n && i < 512; ++i) if (g_armMask[i]) { M4 b; memcpy(b.m, bones[i].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1; Bw[i] = Mul(b, W); Banim[i] = Bw[i]; }
    {   // reload: the arm bones keep their pose (relative to the camera bone) from just before the reload
        // (the helper bones that follow an arm bone too - the left one stretched the sleeve when only the arm was held)
        static M4 relArm[512]; static bool haveRel = false; static void* relWeapon = nullptr;
        bool rl = ReloadActive();
        for (int i = 0; i < n && i < 512; ++i) if (g_follow[i] >= 0) { M4 b; memcpy(b.m, bones[i].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1; g_folAnim[i] = Mul(b, W); }
        M4 icw;
        if (Inv(camW, icw)) {
            if (rl && haveRel && relWeapon == g_curWeapon) {
                for (int i = 0; i < n && i < 512; ++i) if (g_armMask[i] || g_follow[i] >= 0) { M4 v = Mul(relArm[i], camW); if (g_armMask[i]) { Bw[i] = v; Banim[i] = v; } else g_folAnim[i] = v; }
            } else if (!rl && g_curWeapon && controls::NormalControls()) {
                for (int i = 0; i < n && i < 512; ++i) { if (g_armMask[i]) relArm[i] = Mul(Bw[i], icw); else if (g_follow[i] >= 0) relArm[i] = Mul(g_folAnim[i], icw); }
                haveRel = true; relWeapon = g_curWeapon;
            }
        }
    }
    // v231 arm smoothing: the game's arm animation steps from frame to frame (a lower animation rate), and the solve
    // takes the upper arm / forearm / twist bones' roll from it, so the arms twitched while the hands held still.
    // The animated clavicle, upper arm, forearm and their twist bones are smoothed relative to the camera bone.
    {
        static M4 relS[512]; static bool haveS[512]; static double lastT = 0; static void* lastSkel = nullptr;
        double tn = GetTickCount64() * 1.0; float dt = lastT > 0 ? (float)((tn - lastT) * 0.001) : 0; lastT = tn;
        if (lastSkel != skel) { for (auto& h : haveS) h = false; lastSkel = skel; }
        M4 icw; bool ok = Inv(camW, icw);
        int chain[2][3] = { { g_slot[19], g_slot[18], g_slot[17] }, { g_slot[15], g_slot[14], g_slot[13] } };
        float stepMax = 0;
        if (ok) for (int i = 0; i < n && i < 512; ++i) {
            if (!g_armMask[i]) continue;
            bool arm = false; int par = bones[i].parent;
            for (auto& c : chain) for (int b : c) if (b >= 0 && (i == b || par == b)) arm = true;
            if (!arm || i == g_slot[12] || i == g_slot[20]) continue;
            M4 rel = Mul(Banim[i], icw);
            if (!haveS[i] || dt <= 0 || dt > 0.25f || g_c.armSmooth <= 0.0f) { relS[i] = rel; haveS[i] = true; }
            else {
                float k = 1.0f - expf(-dt / g_c.armSmooth);
                float qa[4], qb[4]; MatQuat(relS[i], qa); MatQuat(rel, qb);
                if (i == g_slot[13] || i == g_slot[17]) { float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]; float ang = 2.0f * acosf(fminf(1.0f, fabsf(d))) * 57.2958f; if (ang > stepMax) stepMax = ang; }
                float q[4]; QuatNlerp(qa, qb, k, q);
                M4 r = QuatMat(q);
                for (int c = 0; c < 3; ++c) { float sc = sqrtf(rel.m[c][0] * rel.m[c][0] + rel.m[c][1] * rel.m[c][1] + rel.m[c][2] * rel.m[c][2]); for (int j = 0; j < 3; ++j) r.m[c][j] *= sc; }
                for (int j = 0; j < 3; ++j) r.m[3][j] = relS[i].m[3][j] + (rel.m[3][j] - relS[i].m[3][j]) * k;
                relS[i] = r;
            }
            if (g_c.armSmooth > 0.0f) { Banim[i] = Mul(relS[i], camW); Bw[i] = Banim[i]; }
        }
        g_dgForeStep = stepMax;
    }
    // v213 monocular: its offset relative to the game's animated right hand, once its raise animation has settled
    if (g_monoAt && !g_monoRelValid && g_monoBone >= 0 && g_monoBone < n && g_monoSince && GetTickCount64() - g_monoSince > 700) {
        int b = g_monoBone; M4 O, Bb, ih;
        memcpy(O.m, g_monoOff, 64); O.m[0][3] = O.m[1][3] = O.m[2][3] = 0; O.m[3][3] = 1;
        if (g_armMask[b]) Bb = Banim[b]; else { memcpy(Bb.m, bones[b].model, 64); Bb.m[0][3] = Bb.m[1][3] = Bb.m[2][3] = 0; Bb.m[3][3] = 1; Bb = Mul(Bb, W); }
        if (Inv(Banim[rh], ih)) {
            g_monoRel = Mul(Mul(O, Bb), ih); g_monoRelValid = true;
            LOG("monocular: place in the right hand %.2f %.2f %.2f (from bone %d)", g_monoRel.m[3][0], g_monoRel.m[3][1], g_monoRel.m[3][2], b);
        }
    }
    // steady hands: the hands keep the pose (relative to the camera) the weapon had shortly after it was drawn, so the
    // game's animations - sprint sway, pulling the gun in near walls - don't turn the hands and the weapon
    {
        static void* wPrev = (void*)1; static ULONGLONG tChange = 0;
        ULONGLONG t = GetTickCount64();
        void* w = g_curWeapon;
        // each weapon's hold is remembered once taken, so it is back the moment the weapon is (after climbing,
        // holstering, a weapon change) - no waiting for a still moment, which never comes while sprinting
        struct HoldCache { void* w; M4 r, l; int axis; float sign; M4 off; bool haveOff; };
        static HoldCache cache[16]; static int nCache = 0;
        if (w != wPrev) {
            wPrev = w; tChange = t; g_relHave = false;
            g_relCached = false; g_curAxis = -1; g_curSign = 1; g_haveOff0 = false;
            for (int i = 0; i < nCache; ++i) if (cache[i].w == w) { g_relR = cache[i].r; g_relL = cache[i].l; g_relHave = true; g_relCached = true; g_curAxis = cache[i].axis; g_curSign = cache[i].sign; g_off0 = cache[i].off; g_haveOff0 = cache[i].haveOff; break; }
        }
        int lhs = g_slot[20];
        // capture only once the hold has settled (drawing / picking up a weapon plays a long animation): the hand's
        // pose relative to the camera must stay still for 0.4 s
        static M4 relPrev; static ULONGLONG tPrevS = 0, stableSince = 0; static bool havePrev = false;
        M4 relNow = Mul(Bw[rh], Cinv);
        bool still = false;
        if (havePrev) {
            float dp[3] = { relNow.m[3][0] - relPrev.m[3][0], relNow.m[3][1] - relPrev.m[3][1], relNow.m[3][2] - relPrev.m[3][2] };
            float dotx = relNow.m[0][0] * relPrev.m[0][0] + relNow.m[0][1] * relPrev.m[0][1] + relNow.m[0][2] * relPrev.m[0][2];
            float doty = relNow.m[1][0] * relPrev.m[1][0] + relNow.m[1][1] * relPrev.m[1][1] + relNow.m[1][2] * relPrev.m[1][2];
            ULONGLONG dtm = t - tPrevS; float lim = 0.004f * (float)(dtm > 0 ? dtm : 1) / 16.0f;
            still = sqrtf(Dot3(dp, dp)) < lim && dotx > 0.9995f && doty > 0.9995f;
        }
        relPrev = relNow; tPrevS = t; havePrev = true;
        if (!still) stableSince = t;
        // When a hold may be taken (or the remembered one renewed):
        //  - only with the game's normal on-foot controls (not the phone, watch, map, menus, cut scenes);
        //  - the weapon's barrel must point roughly where the view points (the relaxed gun in a cease-fire town
        //    points up) - the barrel axis is learned from normal holds and saved (WeaponFwdAxis);
        //  - a renewal must stay within 20 deg of the remembered hold.
        // Taken holds are saved per weapon type (the HUD reticle's name id) in the ini [Holds], so a weapon first
        // drawn in a town or during a phone call still gets its normal hold from an earlier game.
        auto rotAngle = [](const M4& x, const M4& y) {   // angle of the rotation between two frames (rows 0-2)
            float tr = 0; for (int i = 0; i < 3; ++i) { float nx = sqrtf(Dot3(x.m[i], x.m[i])), ny = sqrtf(Dot3(y.m[i], y.m[i]));
                if (nx > 1e-6f && ny > 1e-6f) tr += Dot3(x.m[i], y.m[i]) / (nx * ny); }
            return acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
        };
        uint32_t key = w ? g_typeKey : 0xFFFFFFFFu;   // 0xFFFFFFFF = empty hands; 0 = weapon type unknown (not worked out yet)
        static uint32_t loadedKey = 0; static void* loadedW = nullptr;
        if (g_relHave && loadedW == w && loadedKey && key && key != loadedKey && t - tChange < 2000) { g_relHave = false; g_curAxis = -1; LOG("arms: weapon %p hold key settled on %08x - its own hold is used", w, key); }   // the key changed while settling
        if (!g_relHave && t - tChange > 250 && key) {   // a saved hold for this weapon type
            M4 r, l; int ax; float sg;
            loadedKey = key; loadedW = w;
            if (LoadHold(key, r, l, &ax, &sg)) { g_relR = r; g_relL = l; g_relHave = true; g_relCached = true; if (g_curAxis < 0) { g_curAxis = ax; g_curSign = sg; } LOG("arms: saved hold %08x used for weapon %p", key, w); }
        }
        if (!g_relHave && t - tChange > 150 && !controls::NormalControls()) {   // the phone (or another gadget with its own controls)
            M4 r, l; if (LoadHold(0xFFFFFFFFu, r, l, nullptr, nullptr)) { g_relR = r; g_relL = l; g_relHave = true; g_relCached = true; LOG("arms: empty-hands hold used for item %p (phone / gadget)", w); }
        }
        // A hold is taken once per weapon (type) and then kept: automatic renewals picked up the game's special poses
        // (town, phone, map). Left grip + A takes the current hold again on request (when one looks wrong).
        static ULONGLONG recapUntil = 0;
        if (controls::TakeRecapture()) { recapUntil = t + 3000; LOG("arms: hold for weapon %p will be taken again (left grip + A)", w); xr::Haptic(1, 0.4f, 0.05f); }
        bool manual = recapUntil > t;
        g_relCached = false;   // no automatic renewals
        bool due = g_c.steadyHands && (!g_relHave || manual) && t - tChange > 300 && t - stableSince > (manual ? 200u : 400u) && !f.map && controls::NormalControls();
        // The weapon's own barrel: the weapon entity axis that points along the view in a normal hold differs per
        // weapon type (rifles +X, the pistol -Z), so it is learned per type and saved with the hold. A hold is taken
        // or renewed only while that axis points along the view (the relaxed gun of a cease-fire town points up);
        // with the barrel along the view any turn of the hand is fine (e.g. a gun brought back rolled in the hand
        // after a phone call). Without a known barrel axis (empty hands, a new type) a renewal must stay within 20 deg.
        float dAx[3] = { 0, 0, 0 }; bool haveAx = false;
        if (due && w) {
            uint8_t* went = nullptr;
            uint8_t* wh = !IsBadReadPtr((uint8_t*)w + 8, 4) ? *(uint8_t**)((uint8_t*)w + 8) : nullptr;   // component -> owner handle {id, id, ref, entity}
            if (wh && !IsBadReadPtr(wh, 16)) went = *(uint8_t**)(wh + 12);
            if (went && !IsBadReadPtr(went + 0x30, 64)) {
                float rows[3][3];
                for (int k = 0; k < 3; ++k) { for (int j = 0; j < 3; ++j) rows[k][j] = *(float*)(went + 0x30 + k * 16 + j * 4); Normalize(rows[k]); dAx[k] = Dot3(rows[k], cf); }
                haveAx = true;
            }
        }
        bool barrelOk = false;
        if (due && haveAx && g_curAxis >= 0 && !manual) {
            float c = g_curSign * dAx[g_curAxis];
            barrelOk = c > 0.85f;
            if (!barrelOk) {
                static ULONGLONG lastLog = 0;
                if (t - lastLog > 3000) { lastLog = t; LOG("arms: hold for weapon %p not taken: barrel %.0f deg off the view (a special pose, e.g. a cease-fire town)", w, acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 57.2958f); }
                due = false;
            }
        }
        if (due && g_relCached && !barrelOk) {
            float ang = rotAngle(g_relR, relNow);
            if (ang > 20.0f) {
                static ULONGLONG lastLog = 0;
                if (t - lastLog > 3000) { lastLog = t; LOG("arms: hold for weapon %p not renewed: %.1f deg from the remembered one", w, ang); }
                due = false; g_relCached = false;   // keep the remembered hold until the next weapon change
            }
        }
        if (due && haveAx && g_curAxis < 0) {   // learn this weapon type's barrel axis from a normal hold
            int best = 0; for (int k = 1; k < 3; ++k) if (fabsf(dAx[k]) > fabsf(dAx[best])) best = k;
            if (fabsf(dAx[best]) > 0.85f) { g_curAxis = best; g_curSign = dAx[best] > 0 ? 1.0f : -1.0f; LOG("arms: weapon %p (type %08x) barrel axis %c%c", w, key, g_curSign > 0 ? '+' : '-', "XYZ"[best]); }
        }
        if (due) {
            if (manual) { recapUntil = 0; g_haveOff0 = false; xr::Haptic(1, 0.6f, 0.08f); LOG("arms: hold for weapon %p taken again on request", w); }
            if (g_relCached) LOG("arms: steady hold renewed for weapon %p (%.1f s after the change, %.1f deg from the remembered one)", w, (t - tChange) / 1000.0, rotAngle(g_relR, relNow));
            else LOG("arms: steady hold taken for weapon %p (type %08x, %.1f s after the change)", w, key, (t - tChange) / 1000.0);
            g_relCached = false;
            g_relR = relNow;
            if (lhs >= 0 && lhs < n && g_armMask[lhs]) g_relL = Mul(Bw[lhs], Cinv);
            g_relHave = true;
            int slot = -1; for (int i = 0; i < nCache; ++i) if (cache[i].w == w) slot = i;
            if (slot < 0) slot = nCache < 16 ? nCache++ : (int)(t % 16);
            // the weapon's place in the hand now, and the forearms relative to the hands (for special poses)
            // (kept from the weapon's first hold: a renewal after a phone call may already see the weapon turned)
            { M4 ww, ih;
              if (!g_haveOff0 && w && g_havePrevFinal && WeaponWorld(w, ww) && Inv(g_prevFinalRh, ih)) { g_off0 = Mul(ww, ih); g_haveOff0 = true; } }
            for (int sd = 0; sd < 2; ++sd) {
                int fo = g_slot[sd ? 13 : 17], ha = g_slot[sd ? 12 : 20]; M4 ih;
                if (fo >= 0 && ha >= 0 && fo < n && ha < n && g_armMask[fo] && g_armMask[ha] && Inv(Banim[ha], ih)) { g_foreRel[sd] = Mul(Banim[fo], ih); g_haveForeRel[sd] = true; }
            }
            cache[slot] = { w, g_relR, g_relL, g_curAxis, g_curSign, g_off0, g_haveOff0 };
            if (key) SaveHold(key, g_relR, g_relL, g_curAxis, g_curSign);
            if (key == 0xFFFFFFFFu) g_freeLDirty = true;
        }
        // the game may leave the weapon turned in the hand (after a phone call): turn the hand so that the weapon sits
        // as it did when the hold was taken (weapon place in the hand = weapon world * inverse of last frame's hand)
        M4 relUse = g_relR;
        if (g_c.steadyHands && g_relHave && !f.map && g_haveOff0 && !g_offBroken && g_c.weaponRollFix && w && g_havePrevFinal) {
            M4 ww, ih;
            if (WeaponWorld(w, ww) && Inv(g_prevFinalRh, ih)) {
                M4 offNow = Mul(ww, ih), ion;
                static M4 offPrev; static void* offPrevW = nullptr; static int jumps = 0, frames = 0;
                if (offPrevW == w) { float dj = rotAngle(offPrev, offNow); if (dj > 5.0f) ++jumps; if (++frames >= 90) { if (jumps > 20) { g_offBroken = true; LOG("arms: weapon place in the hand is not steady (%d jumps in %d frames) - correction off", jumps, frames); } jumps = frames = 0; } }
                offPrev = offNow; offPrevW = w;
                float ang = rotAngle(g_off0, offNow);
                if (ang > 3.0f && ang < 120.0f && Inv(offNow, ion)) {
                    M4 D = Mul(ion, g_off0); D.m[3][0] = D.m[3][1] = D.m[3][2] = 0;
                    relUse = Mul(D, g_relR);
                    static ULONGLONG lastLog = 0; if (t - lastLog > 5000) { lastLog = t; LOG("arms: weapon %p turned %.1f deg in the hand - hand turned to match", w, ang); }
                }
            }
        }
        if (g_c.steadyHands && g_relHave && !f.map) { M4 inv; if (Inv(Bw[rh], inv)) Map = Mul(Mul(inv, relUse), Am); }
    }
    // right hand lands on the controller grip (plus a tunable offset in the controller frame)
    float h0[3] = { Bw[rh].m[3][0], Bw[rh].m[3][1], Bw[rh].m[3][2] }, hm[3]; XformPt(Map, h0, hm);
    float tgt[3]; for (int k = 0; k < 3; ++k) tgt[k] = grip[k] + ar[k] * g_c.handOfs[0] + au[k] * g_c.handOfs[1] + ab[k] * g_c.handOfs[2];
    for (int k = 0; k < 3; ++k) Map.m[3][k] += tgt[k] - hm[k];
    if (f.map) {   // map / GPS in the hands: their own angles
        float axf[3] = { -ab[0], -ab[1], -ab[2] }, axr[3] = { ar[0], ar[1], ar[2] }, axu[3] = { au[0], au[1], au[2] };
        Normalize(axf); Normalize(axr); Normalize(axu);
        M4 T1 = Translate(-tgt[0], -tgt[1], -tgt[2]), T2 = Translate(tgt[0], tgt[1], tgt[2]);
        if (fabsf(g_c.mapRoll) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axf, g_c.mapRoll * 0.0174533f)), T2);
        if (fabsf(g_c.mapAngle) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axr, g_c.mapAngle * 0.0174533f)), T2);
        if (fabsf(g_c.mapYaw) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axu, g_c.mapYaw * 0.0174533f)), T2);
    }
    if (g_curWeapon && controls::PhoneOut()) controls::ClearPhone();   // the call is over once a weapon is back in the hand
    // a phone call: its own angles. Only the phone (and the map controls it starts with) - cut scenes and other scripted
    // scenes ("cinematic", conversations) keep the free-hand angles, as the phone angles turned the hands up and sideways
    const bool phoneMode = (controls::PhoneOut() || controls::MapControls()) && !f.map && !g_curWeapon;
    const float hPitch = phoneMode ? g_c.phonePitch : g_c.freePitch, hYaw = phoneMode ? g_c.phoneYaw : g_c.freeYaw, hRoll = phoneMode ? g_c.phoneRoll : g_c.freeRoll;
    if (!f.map && !g_curWeapon) {   // nothing in the right hand: the free-hand angles (phone-hand angles during a call)
        float axf[3] = { -ab[0], -ab[1], -ab[2] }, axr[3] = { ar[0], ar[1], ar[2] }, axu[3] = { au[0], au[1], au[2] };
        Normalize(axf); Normalize(axr); Normalize(axu);
        M4 T1 = Translate(-tgt[0], -tgt[1], -tgt[2]), T2 = Translate(tgt[0], tgt[1], tgt[2]);
        if (fabsf(hRoll) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axf, hRoll * 0.0174533f)), T2);
        if (fabsf(hPitch) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axr, hPitch * 0.0174533f)), T2);
        if (fabsf(hYaw) > 0.1f) Map = Mul(Mul(Mul(Map, T1), RotMat(axu, hYaw * 0.0174533f)), T2);
    }
    if (f.machete && fabsf(g_c.macheteRoll) > 0.1f) {   // turned about the controller's pointing axis
        float axf[3] = { -ab[0], -ab[1], -ab[2] }; Normalize(axf);
        Map = Mul(Mul(Mul(Map, Translate(-tgt[0], -tgt[1], -tgt[2])), RotMat(axf, g_c.macheteRoll * 0.0174533f)), Translate(tgt[0], tgt[1], tgt[2]));
    }
    if (f.machete && fabsf(g_c.macheteAngle) > 0.1f) {
        float axr[3] = { ar[0], ar[1], ar[2] }; Normalize(axr);
        Map = Mul(Mul(Mul(Map, Translate(-tgt[0], -tgt[1], -tgt[2])), RotMat(axr, g_c.macheteAngle * 0.0174533f)), Translate(tgt[0], tgt[1], tgt[2]));
    }
    if (f.machete && fabsf(g_c.macheteYaw) > 0.1f) {   // blade angled inward (toward the body's centre line)
        float axu[3] = { au[0], au[1], au[2] }; Normalize(axu);
        Map = Mul(Mul(Mul(Map, Translate(-tgt[0], -tgt[1], -tgt[2])), RotMat(axu, g_c.macheteYaw * 0.0174533f)), Translate(tgt[0], tgt[1], tgt[2]));
    }
    // foregrip: where the animation puts the left hand on this weapon (rifles); pistols/one-handed: next to the right hand
    int lh = g_slot[20];
    bool lhOk = lh >= 0 && lh < n && g_armMask[lh];
    float fg[3] = { tgt[0], tgt[1], tgt[2] };
    bool oneHanded = true;
    if (lhOk) {
        float l0[3] = { Bw[lh].m[3][0], Bw[lh].m[3][1], Bw[lh].m[3][2] };
        float dd[3] = { l0[0] - h0[0], l0[1] - h0[1], l0[2] - h0[2] };
        float span = sqrtf(Dot3(dd, dd));
        oneHanded = span > g_c.twoHandMaxSpan;
        g_lastSpan = span;
        if (!oneHanded) XformPt(Map, l0, fg);
        else for (int k = 0; k < 3; ++k) fg[k] = tgt[k] - ar[k] * 0.02f - au[k] * 0.07f - ab[k] * 0.02f;   // support hand under the grip
    }
    { float r[3]; toRef(fg, r); Lock(); memcpy(g_foregripRef, r, 12); g_foregripValid = true; g_oneHanded = oneHanded; Unlock(); }
    // two-handed: the weapon turns about the right hand so that it points at the left controller like the animation does
    bool grabbing = f.twoHand && f.leftValid;
    bool twoHand = grabbing && !oneHanded;
    float lgrip[3]; if (f.leftValid) toWorld(f.lgripO, lgrip, true);
    if (twoHand) {
        float vr[3] = { fg[0] - tgt[0], fg[1] - tgt[1], fg[2] - tgt[2] }, vc[3] = { lgrip[0] - tgt[0], lgrip[1] - tgt[1], lgrip[2] - tgt[2] };
        Normalize(vr); Normalize(vc);
        float ax[3] = { vr[1] * vc[2] - vr[2] * vc[1], vr[2] * vc[0] - vr[0] * vc[2], vr[0] * vc[1] - vr[1] * vc[0] };
        float sn = sqrtf(Dot3(ax, ax)), cs = Dot3(vr, vc);
        if (sn > 1e-5f) {
            for (int k = 0; k < 3; ++k) ax[k] /= sn;
            float ang = atan2f(sn, cs);
            M4 Rm = RotMat(ax, ang);   // row-vector rotation
            M4 T1 = Translate(-tgt[0], -tgt[1], -tgt[2]), T2 = Translate(tgt[0], tgt[1], tgt[2]);
            Map = Mul(Mul(Mul(Map, T1), Rm), T2);
        }
    }
    // shots follow the sight line of the re-posed weapon
    // (steady hands: from the camera as it sat on the hand when the hold was taken, carried by the re-pose - not
    // from the game's camera now, which sways while sprinting and would make the aiming dot and shots sway with it)
    { float so[3], sf[3]; XformPt(Map, co, so); XformDir(Map, cf, sf);
      if (g_c.steadyHands && g_relHave && !f.map) {
          M4 ir; if (Inv(g_relR, ir)) {
              M4 V = Mul(Mul(ir, Bw[rh]), Map);   // the capture-time camera, re-posed with the weapon
              for (int k = 0; k < 3; ++k) { so[k] = V.m[3][k]; sf[k] = -V.m[2][k]; }
          }
      }
      Normalize(sf);
      float sr[3], sfr[3]; Put3(sfr, f.game.ReferenceDirection(Read3(sf))); toRef(so, sr);
      float cro[3], crd[3]; Put3(crd, f.game.ReferenceDirection(Read3(cf))); toRef(co, cro);   // raw native camera line: turrets still aim along it
      const bool authority = f.handAuthority && f.weapon && f.weapon == g_curWeapon && xr::InputCurrent(f.source,1);
      Lock(); if (g_shotWeapon != f.weapon) g_scopeTick = 0;
      g_shotSource = f.source; g_shotWeapon = f.weapon;
      memcpy(g_shotO, so, 12); memcpy(g_shotF, sf, 12); memcpy(g_camFwdNow, cf, 12); g_shotValid = authority; memcpy(g_rayRefO, sr, 12); memcpy(g_rayRefD, sfr, 12);
      memcpy(g_camRefO, cro, 12); memcpy(g_camRefD, crd, 12); g_rayTick = GetTickCount(); Unlock(); }
    // rigid re-pose of both arm chains
    for (int i = 0; i < n && i < 512; ++i) if (g_armMask[i]) Bw[i] = Mul(Bw[i], Map);
    // ---- IK: shoulders from the head, elbows solved, hands on their targets
    if (g_c.ik && f.headValid) {
        float hp[3], hf[3], hr[3], up[3] = { f.game.U.x, f.game.U.y, f.game.U.z };
        toWorld(f.headO, hp, true); toWorld(f.headF, hf, false);
        float d = Dot3(hf, up); for (int k = 0; k < 3; ++k) hf[k] -= up[k] * d; Normalize(hf);
        hr[0] = hf[1] * up[2] - hf[2] * up[1]; hr[1] = hf[2] * up[0] - hf[0] * up[2]; hr[2] = hf[0] * up[1] - hf[1] * up[0]; Normalize(hr);
        if (Dot3(hr, f.game.R) < 0) for (int k = 0; k < 3; ++k) hr[k] = -hr[k];
        for (int side = 0; side < 2; ++side) {
            int clav = g_slot[side ? 15 : 19], upa = g_slot[side ? 14 : 18], fore = g_slot[side ? 13 : 17], hand = g_slot[side ? 12 : 20];
            if (clav < 0 || upa < 0 || fore < 0 || hand < 0 || clav >= n || upa >= n || fore >= n || hand >= n) continue;
            float sg = side ? 1.0f : -1.0f;
            bool leftFree = false; M4 MapLs; float rollAx[3] = { 0, 0, 0 }, rollP[3] = { 0, 0, 0 };
            // left hand: follows the left controller unless it holds a rifle's foregrip; on a pistol it joins the right hand
            if (!side && !(grabbing && !oneHanded)) {
                if (!f.leftValid || (!g_c.freeLeftHand && !grabbing)) continue;
                float lr[3], lu[3], lb[3], lo[3]; toWorld(f.laimR, lr, false); toWorld(f.laimU, lu, false); toWorld(f.laimB, lb, false); toWorld(f.laimO, lo, true);
                M4 AL = FromRows(lr, lu, lb, lo);
                M4 MapL = Mul(Cinv, AL);
                MapLs = MapL;
                M4 handAnim; { M4 b; memcpy(b.m, bones[hand].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1; handAnim = Mul(b, W); }
                bool steadyL = g_c.steadyHands && g_relHave && !f.map;
                // the free left hand (not on the weapon) always takes the empty-hands pose: a weapon's hold carries the
                // left hand of that weapon's animation (e.g. shaped for a rifle's foregrip), which looked twisted on a
                // free controller
                M4 relLUse = g_relL;
                if (steadyL && !grabbing && g_curWeapon) {
                    static M4 freeL; static bool haveFreeL = false; static ULONGLONG tryT = 0;
                    ULONGLONG tn = GetTickCount64();
                    if (g_freeLDirty) { haveFreeL = false; g_freeLDirty = false; }
                    if (!haveFreeL && tn - tryT > 2000) { tryT = tn; M4 r; if (LoadHold(0xFFFFFFFFu, r, freeL, nullptr, nullptr)) { haveFreeL = true; LOG("arms: free left hand uses the empty-hands pose"); } }
                    if (haveFreeL) relLUse = freeL;
                }
                M4 newHand = steadyL ? Mul(relLUse, AL) : Mul(handAnim, MapL);
                if (steadyL) { M4 ih; if (Inv(handAnim, ih)) MapLs = Mul(ih, newHand); }
                for (int k = 0; k < 3; ++k) newHand.m[3][k] = grabbing ? fg[k] : lgrip[k] - lr[k] * g_c.handOfs[0] + lu[k] * g_c.handOfs[1] + lb[k] * g_c.handOfs[2];
                M4 inv; if (!Inv(Bw[hand], inv)) continue;
                M4 D = Mul(inv, newHand);
                if (fabsf(g_c.leftHandRoll) > 0.1f) {   // turn the hand about the controller's pointing axis (clockwise seen from behind)
                    float ax[3] = { -lb[0], -lb[1], -lb[2] }; Normalize(ax);
                    float hp0[3] = { newHand.m[3][0], newHand.m[3][1], newHand.m[3][2] };
                    D = Mul(Mul(Mul(D, Translate(-hp0[0], -hp0[1], -hp0[2])), RotMat(ax, g_c.leftHandRoll * 0.0174533f)), Translate(hp0[0], hp0[1], hp0[2]));
                    memcpy(rollAx, ax, 12); memcpy(rollP, hp0, 12);
                }
                if (fabsf(g_c.leftHandYaw) > 0.1f) {    // turn the hand inward (fingers toward the body's centre line) about the controller's up axis
                    float ax[3] = { -lu[0], -lu[1], -lu[2] }; Normalize(ax);
                    float hp0[3] = { newHand.m[3][0], newHand.m[3][1], newHand.m[3][2] };
                    D = Mul(Mul(Mul(D, Translate(-hp0[0], -hp0[1], -hp0[2])), RotMat(ax, g_c.leftHandYaw * 0.0174533f)), Translate(hp0[0], hp0[1], hp0[2]));
                }
                if (f.map) {   // the map hand: the same angles, mirrored for the left side
                    float axf[3] = { -lb[0], -lb[1], -lb[2] }, axr[3] = { lr[0], lr[1], lr[2] }, axu[3] = { lu[0], lu[1], lu[2] };
                    Normalize(axf); Normalize(axr); Normalize(axu);
                    float hp0[3] = { newHand.m[3][0], newHand.m[3][1], newHand.m[3][2] };
                    M4 T1 = Translate(-hp0[0], -hp0[1], -hp0[2]), T2 = Translate(hp0[0], hp0[1], hp0[2]);
                    if (fabsf(g_c.mapRoll) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axf, -g_c.mapRoll * 0.0174533f)), T2);
                    if (fabsf(g_c.mapAngle) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axr, g_c.mapAngle * 0.0174533f)), T2);
                    if (fabsf(g_c.mapYaw) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axu, -g_c.mapYaw * 0.0174533f)), T2);
                } else if (!grabbing) {   // the free left hand: the free-hand angles, mirrored for the left side
                    float axf[3] = { -lb[0], -lb[1], -lb[2] }, axr[3] = { lr[0], lr[1], lr[2] }, axu[3] = { lu[0], lu[1], lu[2] };
                    Normalize(axf); Normalize(axr); Normalize(axu);
                    float hp0[3] = { newHand.m[3][0], newHand.m[3][1], newHand.m[3][2] };
                    M4 T1 = Translate(-hp0[0], -hp0[1], -hp0[2]), T2 = Translate(hp0[0], hp0[1], hp0[2]);
                    if (fabsf(hRoll) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axf, -hRoll * 0.0174533f)), T2);
                    if (fabsf(hPitch) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axr, hPitch * 0.0174533f)), T2);
                    if (fabsf(hYaw) > 0.1f) D = Mul(Mul(Mul(D, T1), RotMat(axu, -hYaw * 0.0174533f)), T2);
                }
                leftFree = true;
                SubtreeXform(Bw, bones, n, hand, D);
            }
            // Each arm turns with its own controller only: the clavicle and upper arm keep the body's (animated) orientation
            // instead of the right controller's, and the free left forearm takes the left controller's orientation, so
            // rolling one controller turns that hand and forearm together and never the other arm.
            if (g_c.armsIndependent) {
                Bw[clav] = Banim[clav]; Bw[upa] = Banim[upa];
                for (int i = 0; i < n && i < 512; ++i) {
                    if (!g_armMask[i] || i == upa || i == fore || i == hand) continue;
                    int par = bones[i].parent;
                    if (par == clav || par == upa) Bw[i] = Banim[i];
                }
                if (leftFree) {
                    M4 Rf = Translate(0, 0, 0);
                    if (fabsf(g_c.leftHandRoll) > 0.1f && Dot3(rollAx, rollAx) > 0.5f)
                        Rf = Mul(Mul(Translate(-rollP[0], -rollP[1], -rollP[2]), RotMat(rollAx, g_c.leftHandRoll * g_c.leftForearmTwist * 0.0174533f)), Translate(rollP[0], rollP[1], rollP[2]));
                    Bw[fore] = Mul(Mul(Banim[fore], MapLs), Rf);
                    for (int i = 0; i < n && i < 512; ++i)
                        if (g_armMask[i] && i != hand && bones[i].parent == fore) Bw[i] = Mul(Mul(Banim[i], MapLs), Rf);
                }
            }
            // special controls (phone, watch, ...): the wrist keeps its normal bend - the forearm sits on the hand as in
            // a normal hold, not as the game's phone animation bends it
            if (!controls::NormalControls() && g_haveForeRel[side] && (side || leftFree)) {
                M4 iaf; bool okf = Inv(Banim[fore], iaf);
                Bw[fore] = Mul(g_foreRel[side], Bw[hand]);
                if (okf) for (int i = 0; i < n && i < 512; ++i)
                    if (g_armMask[i] && i != hand && bones[i].parent == fore) Bw[i] = Mul(Mul(Banim[i], iaf), Bw[fore]);
            }
            float S[3], E0[3], T[3], P0[3];
            for (int k = 0; k < 3; ++k) { P0[k] = Bw[upa].m[3][k]; E0[k] = Bw[fore].m[3][k]; T[k] = Bw[hand].m[3][k]; }
            float E0u[3];   // the elbow as the (old) upper arm sees it
            { M4 iua; float lp[3], fp[3] = { Banim[fore].m[3][0], Banim[fore].m[3][1], Banim[fore].m[3][2] };
              if (Inv(Banim[upa], iua)) { XformPt(iua, fp, lp); XformPt(Bw[upa], lp, E0u); } else memcpy(E0u, E0, 12); }
            for (int k = 0; k < 3; ++k) S[k] = hp[k] + hr[k] * (sg * g_c.shoulderSide) - up[k] * g_c.shoulderDown - hf[k] * g_c.shoulderBack;
            // bone lengths from the animated pose (the free left hand has already been moved to the controller)
            float a, b;
            { float u0[3] = { Banim[upa].m[3][0], Banim[upa].m[3][1], Banim[upa].m[3][2] }, f0[3] = { Banim[fore].m[3][0], Banim[fore].m[3][1], Banim[fore].m[3][2] },
                    h0b[3] = { Banim[hand].m[3][0], Banim[hand].m[3][1], Banim[hand].m[3][2] };
              a = Dist(u0, f0); b = Dist(f0, h0b); }
            float dv[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] }; float dl = sqrtf(Dot3(dv, dv));
            if (dl < 1e-4f) continue;
            float nrm[3] = { dv[0] / dl, dv[1] / dl, dv[2] / dl };
            float maxL = 0.995f * (a + b), minL = fabsf(a - b) + 0.05f;
            if (dl > maxL) {
                if (!side && !grabbing) {   // free left hand out of reach: keep the shoulder on the body, stop the hand short
                    float nT[3]; for (int k = 0; k < 3; ++k) nT[k] = S[k] + nrm[k] * maxL;
                    M4 tr = Translate(nT[0] - T[0], nT[1] - T[1], nT[2] - T[2]);
                    SubtreeXform(Bw, bones, n, hand, tr);
                    for (int k = 0; k < 3; ++k) T[k] = nT[k];
                } else for (int k = 0; k < 3; ++k) S[k] = T[k] - nrm[k] * maxL;
                dl = maxL;
            }
            if (dl < minL) { for (int k = 0; k < 3; ++k) S[k] = T[k] - nrm[k] * minL; dl = minL; }
            float cosA = (a * a + dl * dl - b * b) / (2 * a * dl); cosA = cosA > 1 ? 1 : cosA < -1 ? -1 : cosA;
            float sinA = sqrtf(1 - cosA * cosA);
            // elbow hint: down, out to the side, a little back
            float pole[3]; for (int k = 0; k < 3; ++k) pole[k] = -up[k] + hr[k] * sg * 0.6f - hf[k] * 0.3f;
            float pd = Dot3(pole, nrm); for (int k = 0; k < 3; ++k) pole[k] -= nrm[k] * pd; Normalize(pole);
            float E[3]; for (int k = 0; k < 3; ++k) E[k] = S[k] + nrm[k] * (a * cosA) + pole[k] * (a * sinA);
            // upper arm and forearm turned onto the new segments, twist bones follow their segment
            M4 oldU = Bw[upa], oldF = Bw[fore];
            float H0[3]; for (int k = 0; k < 3; ++k) H0[k] = E0[k] + (Banim[hand].m[3][k] - Banim[fore].m[3][k]);   // old forearm direction
            { // the rigid re-pose rotated the forearm; recover its direction from the bone itself
              float d0[3] = { Banim[hand].m[3][0] - Banim[fore].m[3][0], Banim[hand].m[3][1] - Banim[fore].m[3][1], Banim[hand].m[3][2] - Banim[fore].m[3][2] }, dl0[3];
              M4 af; if (Inv(Banim[fore], af)) { XformDir(af, d0, dl0); XformDir(oldF, dl0, d0); for (int k = 0; k < 3; ++k) H0[k] = E0[k] + d0[k]; } }
            M4 newU = AimSegment(oldU, P0, E0u, S, E), newF = AimSegment(oldF, E0, H0, E, T);
            if (g_c.armsIndependent && g_c.upperArmTwist > 0.0f) {
                // the upper arm follows part of the forearm's turn (relative to how they sit in the animation),
                // measured about the upper arm's axis on the bone axis closest to the elbow's hinge
                float ur[3], fr[3], n0[3];
                for (int k = 0; k < 3; ++k) { ur[k] = Banim[fore].m[3][k] - Banim[upa].m[3][k]; fr[k] = Banim[hand].m[3][k] - Banim[fore].m[3][k]; }
                n0[0] = ur[1] * fr[2] - ur[2] * fr[1]; n0[1] = ur[2] * fr[0] - ur[0] * fr[2]; n0[2] = ur[0] * fr[1] - ur[1] * fr[0];
                static int hingeRow[2] = { -1, -1 };
                if (Dot3(n0, n0) > 1e-8f) { Normalize(n0); int best = 0; float bv = -1;
                    for (int r = 0; r < 3; ++r) { float row[3] = { Banim[upa].m[r][0], Banim[upa].m[r][1], Banim[upa].m[r][2] }; Normalize(row); float v = fabsf(Dot3(row, n0)); if (v > bv) { bv = v; best = r; } }
                    hingeRow[side] = best; }
                int kr = hingeRow[side];
                if (kr >= 0) {
                    auto twist = [&](const M4& U, const M4& F, const float* axis) {
                        float ax[3] = { axis[0], axis[1], axis[2] }; Normalize(ax);
                        float a[3] = { U.m[kr][0], U.m[kr][1], U.m[kr][2] }, b[3] = { F.m[kr][0], F.m[kr][1], F.m[kr][2] };
                        float da = Dot3(a, ax), db = Dot3(b, ax);
                        for (int k = 0; k < 3; ++k) { a[k] -= ax[k] * da; b[k] -= ax[k] * db; }
                        float c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
                        return atan2f(Dot3(c, ax), Dot3(a, b));
                    };
                    float u1[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
                    float d = twist(newU, newF, u1) - twist(Banim[upa], Banim[fore], ur);
                    while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
                    Normalize(u1);
                    newU = Mul(Mul(Mul(newU, Translate(-S[0], -S[1], -S[2])), RotMat(u1, d * g_c.upperArmTwist)), Translate(S[0], S[1], S[2]));
                }
            }
            M4 iu, iF; if (!Inv(oldU, iu) || !Inv(oldF, iF)) continue;
            M4 Du = Mul(iu, newU), Df = Mul(iF, newF);
            for (int i = 0; i < n && i < 512; ++i) {
                if (!g_armMask[i] || i == hand || i == upa || i == fore) continue;
                int par = bones[i].parent;
                if (par == upa && i != fore) Bw[i] = Mul(Bw[i], Du);          // upper-arm twist bones
                else if (par == fore && i != hand) Bw[i] = Mul(Bw[i], Df);    // forearm twist bones
            }
            if (!g_c.armsIndependent && leftFree && fabsf(g_c.leftHandRoll) > 0.1f && g_c.leftForearmTwist > 0.0f) {
                // share the hand's roll with the forearm twist bones so the wrist doesn't look wrung
                float ax[3] = { T[0] - E[0], T[1] - E[1], T[2] - E[2] }; Normalize(ax);
                M4 R = Mul(Mul(Translate(-E[0], -E[1], -E[2]), RotMat(ax, g_c.leftHandRoll * g_c.leftForearmTwist * 0.0174533f)), Translate(E[0], E[1], E[2]));
                for (int i = 0; i < n && i < 512; ++i)
                    if (g_armMask[i] && i != hand && i != fore && bones[i].parent == fore) Bw[i] = Mul(Bw[i], R);
            }
            Bw[upa] = newU; Bw[fore] = newF;
            for (int k = 0; k < 3; ++k) Bw[clav].m[3][k] += S[k] - P0[k];       // clavicle follows the shoulder
        }
    }
    // helper bones follow their arm bone: new = old * inv(leaderAnim) * leaderNew
    // The right arm's helper (bone 105, on the forearm; parented to the root) carries the weapon. Its animated place
    // relative to the hand changes in reloads (the crossbow turned on its side to load a bolt), weapon jams etc. -
    // with FreezeReload it is held where it sat in the hand once the weapon had settled after being drawn.
    for (int i = 0; i < n && i < 512; ++i) {
        int j = g_follow[i]; if (j < 0) continue;
        M4 inv; if (!Inv(Banim[j], inv)) continue;
        M4 bw = g_folAnim[i];
        M4 r = Mul(Mul(bw, inv), Bw[j]);
        bool right = false;
        for (int q = j, g = 0; q >= 0 && q < n && g < 64; q = bones[q].parent, ++g) if (q == g_slot[15]) { right = true; break; }
        if (right && g_c.freezeReload && false) {   // off: bone 105 is a forearm/sleeve helper (locking it bent the right elbow)
            static M4 off[512], offPrev[512]; static void* offW[512]; static bool have[512]; static ULONGLONG wT[512], stillT[512];
            ULONGLONG t = GetTickCount64();
            void* w = g_curWeapon;
            M4 ih, onow; bool okh = Inv(Banim[rh], ih);
            if (okh) onow = Mul(bw, ih);   // animated: helper relative to the animated hand
            if (offW[i] != w) { offW[i] = w; have[i] = false; wT[i] = t; stillT[i] = t; if (okh) offPrev[i] = onow; }
            if (okh && !have[i] && w) {
                float dp = 0, dr = 0; for (int k = 0; k < 3; ++k) { float d = onow.m[3][k] - offPrev[i].m[3][k]; dp += d * d; dr += Dot3(onow.m[k], offPrev[i].m[k]); }
                bool still = sqrtf(dp) < 0.002f && dr > 2.998f;
                offPrev[i] = onow;
                if (!still || !controls::NormalControls()) stillT[i] = t;
                if (t - wT[i] > 1200 && t - stillT[i] > 400) { off[i] = onow; have[i] = true; LOG("arms: weapon %p held fixed in the hand (helper bone %d)", w, i); }
            }
            if (have[i] && w) r = Mul(off[i], Bw[rh]);
        }
        Bw[i] = r;
    }
    g_prevFinalRh = Bw[rh]; g_havePrevFinal = true; InterlockedIncrement(&g_pd.applied);
    {   // diagnostic: which player bone the weapon model is attached to - during a reload (or 3 s after a projectile
        // shot), the weapon's place relative to each bone (last frame's final pose) is tracked; the bones it stays
        // fixed to are logged when the window ends.
        static M4 prevW[512], rel0[512]; static float dev[512]; static bool on = false, have0 = false; static ULONGLONG endT = 0; static int pn = 0;
        static M4 prevAH, relAH0, ww0; static float devAH = 0, devAbs = 0, devAbsRot = 0; static int frames = 0, changed = 0; static M4 wwPrev;
        ULONGLONG t = GetTickCount64();
        bool want = ReloadActive() || t < g_diagUntil;
        M4 ww;
        if (want && !on) { on = true; have0 = false; for (int i = 0; i < 512; ++i) dev[i] = 0; devAH = devAbs = devAbsRot = 0; frames = changed = 0; }
        if (on && pn == n && g_curWeapon && WeaponWorld(g_curWeapon, ww)) {
            for (int i = 0; i < n && i < 512; ++i) {
                M4 ib; if (!Inv(prevW[i], ib)) continue;
                M4 rel = Mul(ww, ib);
                if (!have0) { rel0[i] = rel; continue; }
                float tr = 0; for (int k = 0; k < 3; ++k) tr += Dot3(rel.m[k], rel0[i].m[k]);
                float ang = acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
                float dp[3] = { rel.m[3][0] - rel0[i].m[3][0], rel.m[3][1] - rel0[i].m[3][1], rel.m[3][2] - rel0[i].m[3][2] };
                float d = ang + sqrtf(Dot3(dp, dp)) * 100.0f;   // degrees + cm
                if (d > dev[i]) dev[i] = d;
            }
            {   // also: relative to the hand as the game animated it (before the re-pose), and in the world
                auto dv = [](const M4& a, const M4& b, float* rotOnly) { float tr = 0; for (int k = 0; k < 3; ++k) tr += Dot3(a.m[k], b.m[k]);
                    float ang = acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f; float dp[3] = { a.m[3][0] - b.m[3][0], a.m[3][1] - b.m[3][1], a.m[3][2] - b.m[3][2] };
                    if (rotOnly) *rotOnly = ang; return ang + sqrtf(Dot3(dp, dp)) * 100.0f; };
                M4 iah; if (Inv(prevAH, iah)) { M4 r = Mul(ww, iah); if (!have0) relAH0 = r; else devAH = fmaxf(devAH, dv(r, relAH0, nullptr)); }
                if (!have0) ww0 = ww; else { float ro; float d = dv(ww, ww0, &ro); devAbs = fmaxf(devAbs, d); devAbsRot = fmaxf(devAbsRot, ro); }
                ++frames; if (memcmp(&ww, &wwPrev, sizeof ww) != 0) ++changed; wwPrev = ww;
            }
            have0 = true; endT = t;
        }
        if (on && !want) {
            on = false;
            int best[6] = { -1, -1, -1, -1, -1, -1 };
            for (int k = 0; k < 5; ++k) {
                int bi = -1;
                for (int i = 0; i < n && i < 512; ++i) {
                    bool used = false; for (int q = 0; q < k; ++q) if (best[q] == i) used = true;
                    if (!used && (bi < 0 || dev[i] < dev[bi])) bi = i;
                }
                best[k] = bi;
            }
            LOG("arms: weapon attachment diag - weapon entity %p: vs animated hand %.1f, in the world %.1f (rotation %.0f deg), matrix changed in %d of %d frames", g_curWeapon, devAH, devAbs, devAbsRot, changed, frames);
            LOG("arms: weapon attachment diag - weapon moved (deg+cm) relative to: hand %d %.1f, forearm %d %.1f, bone105 %.1f, camera %d %.1f; steadiest bones: %d %.1f, %d %.1f, %d %.1f, %d %.1f, %d %.1f",
                rh, dev[rh], g_slot[13], g_slot[13] >= 0 ? dev[g_slot[13]] : -1.0f, n > 105 ? dev[105] : -1.0f, cam, dev[cam],
                best[0], best[0] >= 0 ? dev[best[0]] : 0, best[1], best[1] >= 0 ? dev[best[1]] : 0, best[2], best[2] >= 0 ? dev[best[2]] : 0, best[3], best[3] >= 0 ? dev[best[3]] : 0, best[4], best[4] >= 0 ? dev[best[4]] : 0);
        }
        for (int i = 0; i < n && i < 512; ++i) {
            if (g_armMask[i] || g_follow[i] >= 0) prevW[i] = Bw[i];
            else { M4 b; memcpy(b.m, bones[i].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1; prevW[i] = Mul(b, W); }
        }
        pn = n; prevAH = Banim[rh];
    }
    // write back (model space)
    for (int i = 0; i < n && i < 512; ++i) {
        if (!g_armMask[i] && g_follow[i] < 0) continue;
        M4 r = Mul(Bw[i], Winv);
        float* dst = bones[i].model;
        float w3[4] = { dst[3], dst[7], dst[11], dst[15] };
        memcpy(dst, r.m, 64);
        dst[3] = w3[0]; dst[7] = w3[1]; dst[11] = w3[2]; dst[15] = w3[3];
    }
    InterlockedIncrement(&g_applied);
    {   // v231: keep what was written to every arm / helper bone (model space) for the render-time comparison
        Lock(); g_dgN = n < 512 ? n : 512; for (int i = 0; i < g_dgN; ++i) { g_dgWritten[i] = (g_armMask[i] || g_follow[i] >= 0); if (g_dgWritten[i]) memcpy(g_dgModel[i], bones[i].model, 64); } g_dgSkel = skel; Unlock();
    }
    {   // v231 diagnostic: world matrix used, drawn right hand and its controller target (world)
        Lock(); for (int k = 0; k < 3; ++k) { g_dgW[k] = W.m[3][k]; g_dgHand[k] = Bw[rh].m[3][k]; g_dgTarget[k] = grip[k]; } ++g_dgSeq; Unlock();
        // v231: the arm bones as written, relative to the Camera bone: per-frame change (position mm, rotation deg)
        M4 icw; if (Inv(camW, icw)) {
            static M4 prevRel[512]; static bool havePrev = false; static int prevN = 0;
            int ids[10] = { g_slot[13], g_slot[17], g_slot[14], g_slot[18], -1, -1, 105, 104, g_slot[12], g_slot[20] };
            for (int i = 0; i < n && i < 512; ++i) { if (bones[i].parent == g_slot[13] && i != g_slot[12] && ids[4] < 0) ids[4] = i; if (bones[i].parent == g_slot[17] && i != g_slot[20] && ids[5] < 0) ids[5] = i; }
            for (int j = 0; j < 10; ++j) { g_dgArmPos[j] = g_dgArmRot[j] = 0; int b = ids[j]; if (b < 0 || b >= n) continue;
                M4 r = Mul(Bw[b], icw);
                if (havePrev && prevN == n) { float d[3] = { r.m[3][0] - prevRel[b].m[3][0], r.m[3][1] - prevRel[b].m[3][1], r.m[3][2] - prevRel[b].m[3][2] }; g_dgArmPos[j] = sqrtf(Dot3(d, d)) * 1000.0f;
                    float qa[4], qb[4]; MatQuat(prevRel[b], qa); MatQuat(r, qb); float dq = fabsf(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]); g_dgArmRot[j] = 2.0f * acosf(fminf(1.0f, dq)) * 57.2958f; }
                prevRel[b] = r; }
            havePrev = true; prevN = n;
        }
    }
    Lock(); g_frameUsage.haveApplied = true; g_frameUsage.appliedSequence = f.frameSequence;
    ++g_frameUsage.appliedCalls; g_frameUsage.appliedThread = GetCurrentThreadId(); Unlock();
}

// ---------------------------------------------------------------- hand interactions (game thread)
// Grip with a free hand (the left hand when it is not on the weapon; the right hand only without a weapon):
//  - a pickup (ammo, syrette, weapon, diamonds...) within reach is held in the hand; let go over a shoulder to put it
//    in the backpack (the game's own GiveTo + despawn), anywhere else to drop it;
//  - otherwise, if the game's current interaction target (door, vehicle, ...) is within reach, it is used.
struct Handle { uint32_t idLo, idHi, ref; uint8_t* ent; };
static void HandleRelease(Handle* h) {
    if (!h) return;
    if (--h->ref == 0) { typedef void (__fastcall *Rel_t)(Handle*); typedef void (__cdecl *Free_t)(void*);
        ((Rel_t)A(0x1029B7F0))(h); ((Free_t)A(0x10228AD0))(h); }
}
typedef void* (__fastcall *GetComp_t)(void* ent, void* edx, void* type);
static void* GetComp(uint8_t* ent, uintptr_t typeDesc) { return ent ? ((GetComp_t)A(0x104E2CC0))(ent, nullptr, (void*)A(typeDesc)) : nullptr; }
static bool EntPos(uint8_t* ent, float* out) {
    typedef void (__fastcall *Pos_t)(void* ent, void* edx, float* out);
    if (!ent) return false; ((Pos_t)A(0x104DE380))(ent, nullptr, out); return true;
}
// Move an entity: its world matrix is at +0x30 (translation +0x60); 0x104DF440 = SetWorldMatrix(const mat4* aligned,
// component not to notify), thiscall, ret 8. It tells the entity's components and the renderer the entity moved.
// entity matrix from a rotation (quaternion) and a position: 0x10060890 = MatrixFromQuatPos(this = out mat4, quat* (aligned),
// pos*), thiscall, ret 8 (what the character controller uses to place its entity)
static void EntSetPosRot(uint8_t* ent, const float* pos, const float* q, void* except) {
    typedef void (__fastcall *F)(void* ent, void* edx, float* m, void* except);
    typedef float* (__fastcall *Mk)(float* out, void* edx, const float* q, const float* p);
    if (!ent) return;
    alignas(16) float m[16]; alignas(16) float qa[4] = { q[0], q[1], q[2], q[3] }; alignas(16) float pa[4] = { pos[0], pos[1], pos[2], 0 };
    ((Mk)A(0x10060890))(m, nullptr, qa, pa);
    if (!std::isfinite(m[0]) || !std::isfinite(m[12])) return;
    ((F)A(0x104DF440))(ent, nullptr, m, except);
}
static void EntSetPos(uint8_t* ent, const float* pos, void* except) {
    typedef void (__fastcall *F)(void* ent, void* edx, float* m, void* except);
    if (!ent) return;
    alignas(16) float m[16]; memcpy(m, ent + 0x30, 64);
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2];
    ((F)A(0x104DF440))(ent, nullptr, m, except);
}
// Havok rigid body behind an entity (CRigidPhysComponent -> physics entity (+0xF0) -> body wrapper (+0x30) -> hkpRigidBody (+0x68)).
// Wrapper vtable: +0x50 SetLinearVelocity(const float*), +0x54 SetAngularVelocity(const float*) - both wake the body.
// hkpRigidBody: motion type (byte) +0xD8 (6 keyframed, 7 fixed, 9 character), position +0x110, object radius +0x180,
// inverse mass +0x19C, linear velocity +0x1A0, angular velocity +0x1B0.
struct Rigid { uint8_t* rb[8] = {}; int n = 0; int type = 0; float mass = 0, radius = 0; float bmin[3] = {}, bmax[3] = {}; bool box = false; uint32_t vt = 0; };
static bool BodyOf(uint8_t* pe, Rigid& r) {
    // a CPhysRigidEntity (vtable 0x10E64A38): body wrapper +0x30 (vtable 0x10E66EE0) -> hkpRigidBody +0x68;
    // a compound part (vtable 0x10E67958): part object +0x20 (vtable 0x10E67B08) -> hkpRigidBody +0x3C
    if (!pe || IsBadReadPtr(pe, 0x38) || r.n >= 8) return false;
    uint8_t* rb = nullptr;
    uintptr_t vt = *(uintptr_t*)pe;
    if (vt == A(0x10E67958)) {
        uint8_t* o = *(uint8_t**)(pe + 0x20);
        if (o && !IsBadReadPtr(o, 0x40) && *(uintptr_t*)o == A(0x10E67B08)) rb = *(uint8_t**)(o + 0x3C);
    } else {
        uint8_t* w = *(uint8_t**)(pe + 0x30);
        if (w && !IsBadReadPtr(w, 0x6C) && *(uintptr_t*)w == A(0x10E66EE0)) rb = *(uint8_t**)(w + 0x68);
    }
    if (!rb || IsBadReadPtr(rb, 0x1C0)) {
        static int logged = 0; if (logged < 8) { ++logged; LOG("phys: part %p vtbl %08x: no rigid body", (void*)pe, (unsigned)(vt - g_base + 0x10000000)); }
        return false;
    }
    float mi = *(float*)(rb + 0x19C);
    r.rb[r.n] = rb; ++r.n;
    int t = rb[0xD8];
    if (r.n == 1 || (t >= 1 && t <= 5) || t == 8) r.type = t;
    r.mass += mi > 1e-6f ? 1.0f / mi : 1e9f;
    float rad = *(float*)(rb + 0x180); if (rad > r.radius) r.radius = rad;
    return true;
}
// CRigidPhysComponent (vtable 0x10E7B650): one body via +0xF0; CCompoundPhysComponent (0x10E7BFD8): bodies listed at
// +0x40 (count +0x44). Component vtable +0xC4: world bounding box (min*, max*).
static bool GetRigid(uint8_t* ent, Rigid& r) {
    r = Rigid{};
    if (!ent) return false;
    if (*(uint32_t*)A(0x10FDE930) == 0) return false;   // type not registered yet
    uint8_t* comp = (uint8_t*)GetComp(ent, 0x10FDE940);
    if (!comp) return false;
    uintptr_t vt = *(uintptr_t*)comp; r.vt = (uint32_t)(vt - g_base + 0x10000000);
    if (vt == A(0x10E7B650)) BodyOf(*(uint8_t**)(comp + 0xF0), r);
    else if (vt == A(0x10E7BFD8)) {
        uint8_t** arr = *(uint8_t***)(comp + 0x40); uint32_t cnt = *(uint32_t*)(comp + 0x44);
        if (arr && cnt < 64 && !IsBadReadPtr(arr, cnt * 4)) for (uint32_t i = 0; i < cnt; ++i) BodyOf(arr[i], r);
    } else return false;   // static, character, vehicle, ragdoll...
    if (!r.n) { static int logged = 0; if (logged < 6) { ++logged; LOG("phys: comp %p vtbl %08x: no bodies found (+0x40 %p, +0x44 %u)", (void*)comp, r.vt, *(void**)(comp + 0x40), *(uint32_t*)(comp + 0x44)); } return false; }
    typedef void (__fastcall *Box_t)(void* comp, void* edx, float* mn, float* mx);
    float mn[3] = { 0, 0, 0 }, mx[3] = { 0, 0, 0 };
    ((Box_t)(*(void***)comp)[0xC4 / 4])(comp, nullptr, mn, mx);
    if (std::isfinite(mn[0]) && std::isfinite(mx[0]) && mx[0] >= mn[0] && mx[1] >= mn[1] && mx[2] >= mn[2] && mx[0] - mn[0] < 100.0f) {
        memcpy(r.bmin, mn, 12); memcpy(r.bmax, mx, 12); r.box = true;
        float e[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
        r.radius = 0.5f * sqrtf(Dot3(e, e));
    }
    return true;
}
static float BoxDist(const Rigid& r, const float* p) {   // hand to the object's bounding box, shrunk (boxes are loose fits)
    if (!r.box) return 1e9f;
    float d[3];
    for (int k = 0; k < 3; ++k) {
        float c = 0.5f * (r.bmin[k] + r.bmax[k]), h = 0.5f * (r.bmax[k] - r.bmin[k]) * (1.0f - g_c.grabBoxShrink);
        float lo = c - h, hi = c + h;
        d[k] = p[k] < lo ? lo - p[k] : p[k] > hi ? p[k] - hi : 0.0f;
    }
    return sqrtf(Dot3(d, d));
}
static bool RigidDynamic(const Rigid& r) { return r.n && ((r.type >= 1 && r.type <= 5) || r.type == 8); }
// velocities straight on the Havok bodies, as the game does it (0x104A2370): wake (0x10B56190), then the motion's
// setLinearVelocity (vtable +0x40) / setAngularVelocity (+0x44); never on fixed bodies
static void RbSetVel(uint8_t* rb, const float* v, bool angular) {
    if (!rb || rb[0xD8] == 7) return;
    typedef void (__fastcall *Wake_t)(void* rb, void* edx);
    typedef void (__fastcall *SetV_t)(void* motion, void* edx, const float* v);
    ((Wake_t)A(0x10B56190))(rb, nullptr);
    alignas(16) float a[4] = { v[0], v[1], v[2], 0 };
    void** mvt = *(void***)(rb + 0xD0);
    ((SetV_t)mvt[(angular ? 0x44 : 0x40) / 4])(rb + 0xD0, nullptr, a);
}
static void RigidSetVel(const Rigid& r, const float* v, const float* w) {
    for (int i = 0; i < r.n; ++i) { RbSetVel(r.rb[i], v, false); if (w) RbSetVel(r.rb[i], w, true); }
}
// ground (or anything solid) along a ray: the physics world's ray query (world+0x90 -> 0x1049BFC0 via 0x1048DD30,
// filter 0x5BF as the game's own world ray at 0x10831C30). Results: 0x30-byte hits {pos, normal, ..., +0x1C distance}.
static bool GroundRayEx(const float* from, const float* dir, float len, float* hit, float* normal, uint32_t* key, uint32_t mask = 0x5BF) {
    void* world = *(void**)A(0x11613D60);
    if (!world) return false;
    struct Vec { uint8_t* data; uint32_t size, cap; } v{ nullptr, 0, 0 };
    uint32_t filter[4] = { 0, 0, 0, 0 };
    typedef void* (__fastcall *FilterCtor_t)(void* f, void* edx, uint32_t mask, uint32_t z);
    typedef void (__fastcall *FilterDtor_t)(void* f);
    typedef void (__fastcall *Ray_t)(void* world, void* edx, const float* from, const float* delta, Vec* out, void* filter, void* ignore, int flags);
    typedef void (__fastcall *Free_t)(Vec* v);
    float delta[3] = { dir[0] * len, dir[1] * len, dir[2] * len };
    void* fp = ((FilterCtor_t)A(0x1048EC40))(filter, nullptr, mask, 0);
    ((Ray_t)A(0x1048DD30))(world, nullptr, from, delta, &v, fp ? fp : filter, nullptr, 5);
    ((FilterDtor_t)A(0x1021B110))(filter);
    bool ok = false; float best = 1e30f;
    for (uint32_t i = 0; i < v.size && i < 64 && v.data; ++i) {
        const float* h = (const float*)(v.data + i * 0x30);   // {pos, normal, ..., +0x18 body key, +0x1C distance}
        float d = Dist(h, from);
        if (std::isfinite(d) && d < best) { best = d; memcpy(hit, h, 12); if (normal) memcpy(normal, h + 3, 12); if (key) *key = *(const uint32_t*)((const uint8_t*)h + 0x18); ok = true; }
    }
    ((Free_t)A(0x10050F30))(&v);
    return ok;
}
static bool GroundRay(const float* from, const float* dir, float len, float* hit) { return GroundRayEx(from, dir, len, hit, nullptr, nullptr); }
// Where the weapon's sight line (or the scope's, while scoped) meets the world: the bullet ray's own filter (0x5BB,
// as the game's bullets in 0x10113230), from 0.5 m out (clear of the player's own body). No hit: 300 m out.
static float g_aimDist = 0; static DWORD g_aimTick = 0;   // distance along the sight ray to what it points at (game thread)
static bool AimPoint(float o[3], float d[3], float p[3], float* dist) {
    if (!ReadAim(o,d)) return false;
    const float start = 0.5f, len = 300.0f;
    float from[3] = { o[0] + d[0] * start, o[1] + d[1] * start, o[2] + d[2] * start }, hit[3];
    float t = len;
    if (GroundRayEx(from, d, len, hit, nullptr, nullptr, 0x5BB)) t = start + Dist(from, hit);
    for (int k = 0; k < 3; ++k) p[k] = o[k] + d[k] * t;
    if (dist) *dist = t;
    return true;
}
// the entity behind a ray hit's body key (world 0x1048E790 -> object, +8 -> handle, as the game does); owned handle
static Handle* HitEntity(uint32_t key) {
    void* world = *(void**)A(0x11613D60);
    if (!world) return nullptr;
    typedef uint8_t* (__fastcall *Find_t)(void* world, void* edx, uint32_t key);
    typedef void (__fastcall *ToHandle_t)(void* obj, void* edx, Handle** out);
    uint8_t* o = ((Find_t)A(0x1048E790))(world, nullptr, key);
    if (!o) return nullptr;
    Handle* h = nullptr; ((ToHandle_t)A(0x1048ED40))(o + 8, nullptr, &h);
    return h;
}
// hand motion history (world), for throws
struct HandHist { float p[16][3]; double t[16]; int n = 0, i = 0; };
static HandHist g_hist[2];
static void HistPush(HandHist& h, const float* p, double t) { memcpy(h.p[h.i], p, 12); h.t[h.i] = t; h.i = (h.i + 1) & 15; if (h.n < 16) ++h.n; }
static bool HistVel(const HandHist& h, double window, float* v) {
    if (h.n < 2) return false;
    int last = (h.i + 15) & 15; int old = last;
    for (int k = 1; k < h.n; ++k) { int j = (last - k + 16) & 15; old = j; if (h.t[last] - h.t[j] >= window) break; }
    double dt = h.t[last] - h.t[old]; if (dt < 0.004) return false;
    for (int k = 0; k < 3; ++k) v[k] = (float)((h.p[last][k] - h.p[old][k]) / dt);
    return true;
}
// objects without a moving physics body (pickups lying in the world) fall and fly with a simple simulation of our own
struct Fall { Handle* h = nullptr; float p[3], v[3], z0 = 0; M4 m; double t0 = 0; bool active = false; };
static Fall g_fall[8];
static double NowSec() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static void SetEntTM(uint8_t* ent, const M4& m) {
    typedef void (__fastcall *SetTM_t)(void* ent, void* edx, const float* m, void* skip);
    ((SetTM_t)A(0x104DF440))(ent, nullptr, &m.m[0][0], nullptr);
}
static void StartFall(Handle* h, const float* v) {
    uint8_t* ent = h ? h->ent : nullptr; if (!ent) { HandleRelease(h); return; }
    Fall* f = nullptr; for (auto& x : g_fall) if (!x.active) { f = &x; break; }
    if (!f) { HandleRelease(g_fall[0].h); g_fall[0] = Fall{}; f = &g_fall[0]; }
    f->h = h; memcpy(f->m.m, ent + 0x30, 64); f->m.m[0][3] = f->m.m[1][3] = f->m.m[2][3] = 0; f->m.m[3][3] = 1;
    for (int k = 0; k < 3; ++k) { f->p[k] = f->m.m[3][k]; f->v[k] = v[k]; }
    f->z0 = f->p[2]; f->t0 = NowSec(); f->active = true;
}
static void UpdateFalls(float dt) {
    for (auto& f : g_fall) {
        if (!f.active) continue;
        uint8_t* ent = f.h ? f.h->ent : nullptr;
        if (!ent || NowSec() - f.t0 > 12.0) { HandleRelease(f.h); f = Fall{}; continue; }
        f.v[2] -= 9.81f * dt;
        float np[3] = { f.p[0] + f.v[0] * dt, f.p[1] + f.v[1] * dt, f.p[2] + f.v[2] * dt };
        float d[3] = { np[0] - f.p[0], np[1] - f.p[1], np[2] - f.p[2] }, len = sqrtf(Dot3(d, d));
        bool landed = false; float hit[3];
        if (len > 1e-5f) {
            float dir[3] = { d[0] / len, d[1] / len, d[2] / len };
            if (GroundRay(f.p, dir, len + 0.05f, hit) && Dist(hit, f.p) <= len + 0.01f) { memcpy(np, hit, 12); landed = true; }
        }
        if (!landed && np[2] < f.z0 - 60.0f) landed = true;   // never found anything below: stop
        memcpy(f.p, np, 12);
        M4 m = f.m;
        if (landed) {   // come to rest upright, keeping the heading
            float fw[3] = { f.m.m[1][0], f.m.m[1][1], 0 }; if (Dot3(fw, fw) < 1e-4f) { fw[0] = f.m.m[0][1]; fw[1] = -f.m.m[0][0]; }
            Normalize(fw); float up[3] = { 0, 0, 1 }, rt[3] = { fw[1], -fw[0], 0 };
            m = FromRows(rt, fw, up, np);
        } else for (int k = 0; k < 3; ++k) m.m[3][k] = np[k];
        SetEntTM(ent, m);
        if (landed) { HandleRelease(f.h); f = Fall{}; }
    }
}
struct Held { Handle* h = nullptr; void* pickup = nullptr; M4 rel; bool active = false; Rigid rg; bool dyn = false; double stuck = 0; };
static Held g_held[2];
static bool g_gripPrev[2] = { false, false };
static volatile bool g_useRequest = false;
static void (*g_useCallback)() = nullptr;

// entities within r of p (synchronous sphere overlap), returned as owned handles
static int OverlapHandles(const float* p, float r, Handle** out, int maxOut, uint32_t mask) {
    static uint32_t buf[1026];
    struct Vec { void* data; uint32_t size, cap; } v{ buf, 0, 0x10001 };
    uint32_t filter[4] = { 0, 0, 0, 0 };
    typedef void (__fastcall *FilterCtor_t)(void* f, void* edx, uint32_t mask, uint32_t z);
    typedef void (__fastcall *FilterDtor_t)(void* f);
    typedef void (__fastcall *Overlap_t)(void* world, void* edx, const float* c, float r, Vec* out, void* filter, uint32_t z);
    typedef void (__fastcall *ToHandle_t)(void* obj, void* edx, Handle** out);
    void* world = *(void**)A(0x11613D60);
    if (!world) return 0;
    ((FilterCtor_t)A(0x1048EC40))(filter, nullptr, mask, 0);
    ((Overlap_t)A(0x1048DCB0))(world, nullptr, p, r, &v, filter, 0);
    ((FilterDtor_t)A(0x1021B110))(filter);
    int n = 0;
    for (uint32_t i = 0; i < v.size && i < 1024 && n < maxOut; ++i) {
        uint8_t* obj = ((uint8_t**)v.data)[i];
        if (!obj) continue;
        Handle* h = nullptr; ((ToHandle_t)A(0x1048ED40))(obj + 8, nullptr, &h);
        if (h) out[n++] = h;
    }
    return n;
}


// ---------------------------------------------------------------- grabbing NPCs (ragdolls)
// Every pawn's CCharacterPhysComponent (GetCharPhys 0x1007F3E0, thiscall entity) owns a ragdoll from spawn
// (+0x164 CPhysRagdollEntity -> +0x2C impl -> +0x48 hkaRagdollInstance: +0x8 hkpRigidBody**, +0xC count). Its Driver
// (+0x178, set every frame by the animation component through 0x10547600 thiscall(comp, int)) decides whether the
// ragdoll is in the world: 0 = character proxy only (alive), 1 = limp ragdoll (dead), 2 = limp ragdoll with the proxy
// following its root. A living NPC grabbed by a hand is held at Driver 2 (a knock-down) while held and NpcRagdollTime
// after; the grabbed body part is pulled to the hand with its velocity, the rest follows through the joints.
struct NpcHold { bool on = false; Handle* h = nullptr; uint8_t* comp = nullptr; int body = -1; float off[3] = {}; bool living = false; double strain = 0; };
static NpcHold g_npc[2];
struct NpcForce { uint8_t* comp = nullptr; double until = 0; bool held = false; uint8_t* ent = nullptr; int drv = 1; };
static NpcForce g_npcForce[4];
typedef void (__fastcall *SetDriver_t)(void* comp, void* edx, int d);
static SetDriver_t o_SetDriver = nullptr;
static void __fastcall Hook_SetDriver(void* comp, void* edx, int d) {
    if (d != 1) {
        double t = NowSec();
        for (auto& f : g_npcForce) if (f.comp == comp && (f.held || t < f.until)) {
            // grabbed: Driver 3 = the ragdoll follows the animation (hkaRagdollRigidBodyController) with the character
            // proxy on - the NPC keeps standing while the held limb is pulled by the hand; Driver 1 = knocked down,
            // limp with the proxy off (as a corpse). Coming from the animation (Driver 0), the current animated pose is
            // copied into the ragdoll first (0x10547410 thiscall comp), else the bodies start from a stale pose.
            int cur = *(int*)((uint8_t*)comp + 0x178);
            if (cur == 0) {
                typedef void (__fastcall *Sync_t)(void* comp, void* edx);
                ((Sync_t)A(0x10547410))(comp, nullptr);
            }
            if (cur != f.drv) { static int lg = 0; if (lg < 20) { ++lg; LOG("npc: ragdoll driver %d -> %d (%s)", cur, f.drv, f.drv == 1 ? "knocked down" : "held, standing"); } }
            d = f.drv;
            // the mesh blends the animation with the ragdoll by the graph's PhysWeight (anim+0x1D18, anim =
            // [CAnimationComponent+0x14]); a living NPC has 0 - it stayed upright in its animation while its ragdoll
            // moved. RagdollController (+0x1D20): 1 limp, 2 physics controlled; weight 1 = show the ragdoll.
            if (f.ent && *(uint32_t*)A(0x10FDF180)) {
                uint8_t* ac = (uint8_t*)GetComp(f.ent, 0x10FDF190);
                uint8_t* an = ac && !IsBadReadPtr(ac + 0x14, 4) ? *(uint8_t**)(ac + 0x14) : nullptr;
                if (an && !IsBadWritePtr(an + 0x1D18, 12)) { *(float*)(an + 0x1D18) = f.drv == 1 ? 1.0f : g_c.npcActiveWeight; *(int*)(an + 0x1D20) = f.drv == 1 ? 1 : 2; }
            }
            break;
        }
    }
    o_SetDriver(comp, edx, d);
}
static NpcForce* NpcForceGet(uint8_t* comp) {
    double t = NowSec();
    for (auto& f : g_npcForce) if (f.comp == comp && (f.held || t < f.until)) return &f;
    return nullptr;
}
static void NpcForceSet(uint8_t* comp, bool held, double until, uint8_t* ent = nullptr, int drv = 0) {
    for (auto& f : g_npcForce) if (f.comp == comp) { f.held = held; f.until = until; if (ent) f.ent = ent; if (drv) f.drv = drv; return; }
    for (auto& f : g_npcForce) if (!f.comp || (!f.held && NowSec() > f.until)) { f.comp = comp; f.held = held; f.until = until; f.ent = ent; f.drv = drv ? drv : 1; return; }
}
static bool RagBodies(uint8_t* comp, uint8_t*** bodies, int* n) {
    if (!comp || IsBadReadPtr(comp + 0x164, 0x18)) return false;
    uint8_t* rag = *(uint8_t**)(comp + 0x164); if (!rag || IsBadReadPtr(rag, 0x30)) return false;
    uint8_t* impl = *(uint8_t**)(rag + 0x2C); if (!impl || IsBadReadPtr(impl, 0x50)) return false;
    uint8_t* inst = *(uint8_t**)(impl + 0x48); if (!inst || IsBadReadPtr(inst, 0x10)) return false;
    uint8_t** b = *(uint8_t***)(inst + 8); int c = *(int*)(inst + 0xC);
    if (!b || c <= 0 || c > 64 || IsBadReadPtr(b, c * 4)) return false;
    *bodies = b; *n = c; return true;
}
static int NpcDriver(uint8_t* comp) { return comp && !IsBadReadPtr(comp + 0x178, 4) ? *(int*)(comp + 0x178) : -1; }

// The keyframe controller of Driver 3 (hkaRagdollRigidBodyController at ragdoll impl+0x5C) re-sets each body's
// velocity toward the animation pose every physics step, so the hand's pull on a standing NPC was lost. Its gain
// palette (hkArray of hkaKeyFrameHierarchyUtility::ControlData, 12 floats each: hierarchyGain, velocityDamping,
// accelerationGain, velocityGain, positionGain, ...) is found by shape and softened while the NPC is held.
struct GainSave { float* cd = nullptr; int n = 0; float orig[8][12]; };
static GainSave g_gainSave[2];
static float* FindGainPalette(uint8_t* comp, int* count) {
    uint8_t* rag = comp && !IsBadReadPtr(comp + 0x164, 4) ? *(uint8_t**)(comp + 0x164) : nullptr;
    uint8_t* impl = rag && !IsBadReadPtr(rag + 0x2C, 4) ? *(uint8_t**)(rag + 0x2C) : nullptr;
    uint8_t* ctl = impl && !IsBadReadPtr(impl + 0x5C, 4) ? *(uint8_t**)(impl + 0x5C) : nullptr;
    if (!ctl || IsBadReadPtr(ctl, 0x40)) return nullptr;
    static int dumped = 0;
    if (dumped < 2) { ++dumped; uint32_t* w = (uint32_t*)ctl; LOG("npc: keyframe controller %p: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", (void*)ctl, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]); }
    for (int o = 0; o + 12 <= 0x40; o += 4) {
        float* p = *(float**)(ctl + o); int n = *(int*)(ctl + o + 4);
        if (!p || n < 1 || n > 8 || IsBadReadPtr(p, n * 48)) continue;
        bool ok = true;
        for (int i = 0; i < 12 && ok; ++i) { float v = p[i]; if (!std::isfinite(v) || v < 0.0f || v > 50.0f) ok = false; }
        if (ok && p[2] > 0.0f && p[2] <= 2.0f && p[0] <= 2.0f && p[3] <= 2.0f && p[4] <= 2.0f) {
            static int lg = 0; if (lg < 3) { ++lg; LOG("npc: controller gains at +0x%x (%d): hierarchy %.2f damping %.2f accel %.2f velocity %.2f position %.2f maxLin %.2f maxAng %.2f snap %.2f", o, n, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
            *count = n; return p;
        }
    }
    return nullptr;
}
static void SoftenGains(int hnd, uint8_t* comp, bool on) {
    GainSave& G = g_gainSave[hnd];
    if (on) {
        if (G.cd) return;
        int n = 0; float* p = FindGainPalette(comp, &n);
        if (!p) return;
        G.cd = p; G.n = n; memcpy(G.orig, p, n * 48);
        for (int i = 0; i < n; ++i) { p[i * 12 + 0] *= g_c.npcStiffness; p[i * 12 + 3] *= g_c.npcStiffness; p[i * 12 + 4] *= g_c.npcStiffness; }
    } else if (G.cd) {
        if (!IsBadWritePtr(G.cd, G.n * 48)) memcpy(G.cd, G.orig, G.n * 48);
        G = GainSave{};
    }
}

// The ragdoll impl's frame update (0x104B4D60, thiscall impl, float dt; runs the keyframe controller once per frame
// before the physics sub-steps) re-sets every body's velocity toward the animation pose. Right after it, the held body
// gets the hand's pull back, so the hand wins on that one part while the rest keeps the full animated pose.
// The NPC's walking body follows a pull through its movement input (0x10493920 thiscall(proxy entity, MoveInput*),
// desired velocity at +0), which the AI keeps steering on top of.
struct NpcDrive { uint8_t* impl = nullptr; uint8_t* rb = nullptr; float v[3] = {}; uint8_t* proxy = nullptr; float lead[3] = {}; DWORD tick = 0;
    fc2vr::authority::Stamp source{}; int hand = -1; };
static NpcDrive g_npcDrive[2];
typedef void (__fastcall *RagUpd_t)(void* impl, void* edx, float dt);
static RagUpd_t o_RagUpd = nullptr;
static void __fastcall Hook_RagUpd(void* impl, void* edx, float dt) {
    o_RagUpd(impl, edx, dt);
    NpcDrive snapshot[2]; Lock(); memcpy(snapshot,g_npcDrive,sizeof snapshot); Unlock();
    for (auto& d : snapshot) if (d.impl == impl && d.rb && GetTickCount()-d.tick < 200 && d.hand >= 0 && xr::InputCurrent(d.source,d.hand)) RbSetVel(d.rb,d.v,false);
}
typedef void (__fastcall *ProxyMove_t)(void* proxy, void* edx, float* in);
static ProxyMove_t o_ProxyMove = nullptr;
static void __fastcall Hook_ProxyMove(void* proxy, void* edx, float* in) {
    NpcDrive snapshot[2]; Lock(); memcpy(snapshot,g_npcDrive,sizeof snapshot); Unlock();
    if (in) for (auto& d : snapshot) if (d.proxy == proxy && GetTickCount()-d.tick < 200 && d.hand >= 0 && xr::InputCurrent(d.source,d.hand) && !IsBadWritePtr(in,12)) {
        in[0] += d.lead[0]; in[1] += d.lead[1];
        static int lg = 0; if (lg < 3 && (d.lead[0] != 0 || d.lead[1] != 0)) { ++lg; LOG("npc: led by the hand (%.2f %.2f m/s)", d.lead[0], d.lead[1]); }
    }
    o_ProxyMove(proxy, edx, in);
}
// a pawn (not the player) within reach of the hand: returns its handle (owned) and its character physics component
static Handle* FindNpc(const float* hp, uint8_t* playerEnt, uint8_t** compOut, float* distOut) {
    Handle* hs[64]; int n = OverlapHandles(hp, g_c.npcReach + 0.4f, hs, 64, 0xFFFFFFFFu);
    Handle* best = nullptr; float bestD = 1e9f; uint8_t* bestC = nullptr;
    typedef uint8_t* (__fastcall *CharPhys_t)(void* ent, void* edx);
    for (int i = 0; i < n; ++i) {
        uint8_t* ent = hs[i]->ent;
        uint8_t* comp = ent && ent != playerEnt ? ((CharPhys_t)A(0x1007F3E0))(ent, nullptr) : nullptr;
        bool dup = false; for (int j = 0; j < i; ++j) if (hs[j]->ent == ent) dup = true;
        if (!comp || dup || *(uintptr_t*)comp != A(0x10E72FE0)) continue;
        float d = 1e9f;
        uint8_t** b; int c;
        if (NpcDriver(comp) != 0 && RagBodies(comp, &b, &c)) {   // ragdoll in the world: nearest body part
            for (int k = 0; k < c; ++k) if (b[k] && !IsBadReadPtr(b[k], 0x120)) { float dd = Dist((float*)(b[k] + 0x110), hp); if (dd < d) d = dd; }
        } else {   // standing: an upright capsule from the feet (entity position) up 1.8 m, radius 0.3
            float ep[3]; EntPos(ent, ep);
            float z = hp[2] < ep[2] ? ep[2] : hp[2] > ep[2] + 1.8f ? ep[2] + 1.8f : hp[2];
            float dx = hp[0] - ep[0], dy = hp[1] - ep[1], dz = hp[2] - z;
            d = sqrtf(dx * dx + dy * dy + dz * dz) - 0.3f; if (d < 0) d = 0;
        }
        if (d < bestD) { bestD = d; best = hs[i]; bestC = comp; }
    }
    for (int i = 0; i < n; ++i) if (hs[i] != best) HandleRelease(hs[i]);
    if (best && bestD > g_c.npcReach) { HandleRelease(best); best = nullptr; }
    *compOut = bestC; *distOut = bestD;
    return best;
}
// ---------------------------------------------------------------- climbing
// A grip with the hand against a building (a fixed, non-terrain object) holds on there. While at least one hand holds,
// the player's character controller is moved so that the holding hands stay where they grabbed (pull down = go up),
// with its velocity zeroed (no falling). A hold that ends up below the chest pulls you over the edge (mantle).
// Player: CCharacterPhysComponent (vtable 0x10E72FE0): +0x80 SetPosition(float*), +0x84 GetPosition(float* out),
// +0xA4 SetLinearVelocity(x, y, z).
struct ClimbHand { bool on = false; float anchor[3] = {}, now[3] = {}; float refStart[3] = {}; unsigned gen = 0; };
static unsigned g_climbGen = 0;
static ClimbHand g_climb[2]; static bool g_climbWas = false;
static uint8_t* PlayerCharComp(uint8_t* playerEnt) {
    if (!playerEnt || *(uint32_t*)A(0x10FDE930) == 0) return nullptr;
    uint8_t* c = (uint8_t*)GetComp(playerEnt, 0x10FDE940);
    return (c && *(uintptr_t*)c == A(0x10E72FE0)) ? c : nullptr;
}
static void CharSetVel(uint8_t* c, float x, float y, float z) { typedef void (__fastcall *F)(void*, void*, float, float, float); ((F)(*(void***)c)[0xA4 / 4])(c, nullptr, x, y, z); }
static bool CharGetPos(uint8_t* c, float* p) { typedef float* (__fastcall *F)(void*, void*, float*); alignas(16) float t[4] = {}; ((F)(*(void***)c)[0x84 / 4])(c, nullptr, t); memcpy(p, t, 12); return std::isfinite(t[0]); }
// orientation (quaternion x, y, z, w): +0x8C GetOrientation(out*) ret 4, +0x88 SetOrientation(const q*) ret 4
static void CharGetRot(uint8_t* c, float* q) { typedef float* (__fastcall *F)(void*, void*, float*); alignas(16) float t[4] = { 0, 0, 0, 1 }; ((F)(*(void***)c)[0x8C / 4])(c, nullptr, t); memcpy(q, t, 16); }
static void CharSetRot(uint8_t* c, const float* q) { typedef void (__fastcall *F)(void*, void*, const float*); alignas(16) float t[4] = { q[0], q[1], q[2], q[3] }; ((F)(*(void***)c)[0x88 / 4])(c, nullptr, t); }
static void CharSetPos(uint8_t* c, const float* p) { typedef void (__fastcall *F)(void*, void*, const float*); alignas(16) float t[4] = { p[0], p[1], p[2], 0 }; ((F)(*(void***)c)[0x80 / 4])(c, nullptr, t); }
// is there something climbable right at the hand? (short rays: pointing direction, palm side, down)
static bool ClimbProbe(const float* hp, const float* hr, const float* hb, int hnd, uint8_t* playerEnt) {
    // pointing direction, palm side, down, plus the other axes (a hand pressed against a wall in any way)
    float dirs[7][3] = { { -hb[0], -hb[1], -hb[2] }, { hnd ? -hr[0] : hr[0], hnd ? -hr[1] : hr[1], hnd ? -hr[2] : hr[2] }, { 0, 0, -1 },
                         { hnd ? hr[0] : -hr[0], hnd ? hr[1] : -hr[1], hnd ? hr[2] : -hr[2] }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 } };
    static const float extra[3] = { 0, -1, 0 };
    int tried = 0;
    for (int di = 0; di < 8; ++di) {
        float d[3]; if (di < 7) memcpy(d, dirs[di], 12); else memcpy(d, extra, 12);
        ++tried;
        Normalize(d);
        float from[3] = { hp[0] - d[0] * 0.05f, hp[1] - d[1] * 0.05f, hp[2] - d[2] * 0.05f }, hit[3], n[3]; uint32_t key = 0;
        if (!GroundRayEx(from, d, 0.05f + g_c.climbReach, hit, n, &key)) continue;
        Handle* h = HitEntity(key);
        uint8_t* ent = h ? h->ent : nullptr;
        uint32_t vt = 0; bool ok = false;
        if (ent && ent != playerEnt) {
            uint8_t* comp = *(uint32_t*)A(0x10FDE930) ? (uint8_t*)GetComp(ent, 0x10FDE940) : nullptr;
            vt = comp ? (uint32_t)(*(uintptr_t*)comp - g_base + 0x10000000) : 0;
            Rigid rg;
            if (comp && *(uintptr_t*)comp == A(0x10E7BAE8)) ok = true;                                        // static object (buildings)
            else if (GetRigid(ent, rg) && rg.type == 7) ok = true;                 // fixed parts of structures
        }
        LOG("climb: %s hand ray hit %s (entity %p, phys %08x) %.2f m, normal %.2f %.2f %.2f -> %s", hnd ? "right" : "left",
            ent ? "an object" : "terrain/none", (void*)ent, vt, Dist(hit, hp), n[0], n[1], n[2], ok ? "climbable" : "not climbable");
        HandleRelease(h);
        if (ok) return true;
    }
    LOG("climb: %s hand found no surface within %.2f m (hand at %.2f %.2f %.2f)", hnd ? "right" : "left", g_c.climbReach, hp[0], hp[1], hp[2]);
    return false;
}
// Climb modes (Home key cycles, for testing): 0 = set the controller's position every frame; 1 = steer it with its
// velocity only; 2 = take the controller out of the physics world while climbing and set its position.
static int g_climbMode = -1;
// Weapon away while climbing: the game's own holster lock (what swimming uses). 0x100A0460 builds the event
// CHolsterInventoryLock(this = buffer, bool lock) thiscall ret 4; entity vtable +0x20 sends it to the player entity;
// 0x100A04E0 destroys the event.
// The swim code also sets the inventory's lock flag (0x101260C0 / 0x101260D0 on pawn->pd + 0x4F0) and, while a weapon
// is still out, posts event 0xC64DABBF ("put it away") to the pawn's graph (pawn vtable +0x78, 0x1029DA10 post(hash));
// unlocking posts 0x08AC6FEA (draw it again).
static bool g_climbHolstered = false;
static void PawnPost(uint32_t hash) {
    if (!g_pawn) return;
    typedef void* (__fastcall *Graph_t)(void* pawn, void* edx);
    typedef void (__fastcall *Post_t)(void* g, void* edx, uint32_t hash);
    void* g = ((Graph_t)(*(void***)g_pawn)[0x78 / 4])(g_pawn, nullptr);
    if (g) ((Post_t)A(0x1029DA10))(g, nullptr, hash);
}
static double g_kaLastPost = 0; static int g_kaTries = 0;
static bool g_userHolster = false;   // the player put the weapon away (left grip + B)
static uint8_t* g_lockEnt = nullptr;  // the player entity the holster lock was sent to
static void SendLockEvent(uint8_t* ent, bool lock) {
    typedef void* (__fastcall *Ctor)(void* ev, void* edx, int lock);
    typedef void (__fastcall *Send)(void* ent, void* edx, void* ev);
    typedef void (__fastcall *Dtor)(void* ev, void* edx);
    alignas(16) uint8_t ev[0x40] = {};
    ((Ctor)A(0x100A0460))(ev, nullptr, lock ? 1 : 0);
    ((Send)(*(void***)ent)[0x20 / 4])(ent, nullptr, ev);
    ((Dtor)A(0x100A04E0))(ev, nullptr);
}
static void HolsterKeepAway() {   // each frame while the lock is on: a weapon still out is put away
    double& lastPost = g_kaLastPost; int& tries = g_kaTries;
    static bool wasOut = false; static double redraw[4] = {}; static int nRedraw = 0;
    if (!g_climbHolstered || !g_pawn) { tries = 0; wasOut = false; return; }
    // weapon put away with left grip + B (not climbing): not while the game's own items are up (the safehouse watch,
    // the map, the phone, menus, cut scenes). Other scripted modes (e.g. Mike's bar) still put it away.
    if (!g_climbWas && controls::ItemControls()) return;
    uint8_t* inv = *(uint8_t**)(g_pawn + 0x10) + 0x4F0;
    typedef void* (__fastcall *Q_t)(void* inv);
    bool out = ((Q_t)A(0x10127DA0))(inv) || ((Q_t)A(0x10129E20))(inv);
    double t = NowSec();
    if (out && !wasOut) {   // the weapon came out again although the lock is on (the game draws it again after a cut scene)
        redraw[nRedraw++ & 3] = t;
        if (nRedraw >= 4 && t - redraw[nRedraw & 3] < 10.0 && !g_climbWas) {   // 4 times within 10 s: stop fighting the game
            LOG("climb: the game keeps drawing the weapon again - weapon left in hand (lock off)");
            nRedraw = 0; wasOut = out;
            uint8_t* fl = *(uint8_t**)(inv + 4); if (fl) fl[0x50] = 0;
            if (g_lockEnt) SendLockEvent(g_lockEnt, false);
            g_climbHolstered = false; g_userHolster = false;
            return;
        }
    }
    wasOut = out;
    if (!out) { tries = 0; return; }
    // one request, then time for the put-away animation: posting every frame restarted it endlessly (stuck in a loop)
    if (t - lastPost < 1.2) return;
    lastPost = t;
    if (++tries > 4) {   // the game won't put it away here: stop asking
        if (tries == 5) LOG("climb: the game keeps the weapon out - no more put-away requests until the lock is renewed");
        return;
    }
    // the lock again first: a cut scene may have cleared it (then the game draws the weapon right back)
    { uint8_t* fl = *(uint8_t**)(inv + 4);
      if (fl && !IsBadReadPtr(fl + 0x50, 1) && fl[0x50] == 0) { LOG("climb: holster lock was cleared by the game - set again"); typedef void (__fastcall *Flag_t)(void* inv); ((Flag_t)A(0x101260C0))(inv);
          if (g_lockEnt) SendLockEvent(g_lockEnt, true); } }
    PawnPost(0xC64DABBFu);
}
static void HolsterLock(uint8_t* ent, bool lock, bool draw = true) {
    if (!ent || !g_c.climbHolster || g_climbHolstered == lock) return;
    if (g_pawn) {
        uint8_t* inv = *(uint8_t**)(g_pawn + 0x10) + 0x4F0;
        typedef void (__fastcall *Flag_t)(void* inv);
        ((Flag_t)A(lock ? 0x101260C0 : 0x101260D0))(inv);
        if (!lock && draw) PawnPost(0x08AC6FEAu);
    }
    SendLockEvent(ent, lock); g_lockEnt = ent;
    g_climbHolstered = lock;
    LOG("climb: weapon %s (pawn %p)", lock ? "put away" : "back in hand", (void*)g_pawn);
    if (lock) { g_kaLastPost = 0; g_kaTries = 0; HolsterKeepAway(); }
}
static float g_climbGrace = 0, g_climbGraceT = 0;   // hang time after both grips are released (s), time left
static bool ClimbHanging() { return g_climbGraceT > 0; }
static void CharEnable(uint8_t* c, bool on) { typedef void (__fastcall *F)(void*, void*, int); ((F)(*(void***)c)[0xB4 / 4])(c, nullptr, on ? 1 : 0); }
// The body follows the most recently grabbing hand: its movement in the room since the grab (tracking space, which
// doesn't lag behind the game like the hand's world position does) moves the body the opposite way.
static void ClimbUpdate(uint8_t* playerEnt, const Frame& f, float dt) {
    if (g_climbMode < 0) g_climbMode = g_c.climbMode;
    HolsterKeepAway();
    g_climbGrace = (f.handValid || f.leftValid) ? g_c.climbGrace : 0;
    auto refDirToWorld = [&](const float* r, float* o) { Put3(o, f.game.Direction(Read3(r))); };
    int act = -1;
    for (int i = 0; i < 2; ++i) if (g_climb[i].on && (act < 0 || g_climb[i].gen > g_climb[act].gen)) act = i;
    uint8_t* c = PlayerCharComp(playerEnt);
    static int modeUsed = 0; static unsigned lastGen = 0; static float bodyStart[3], footOfs[3];
    static float holdPos[3]; static bool holdValid = false;
    // turning on the wall (right stick): the game turns the controller; while it is out of the world nothing passes
    // that on to the body, so the body takes the controller's rotation here. If the game doesn't turn it while
    // climbing, we turn it ourselves (ClimbTurnSpeed).
    static float yawPrev = 0; static int stillFrames = 0; static bool ownTurn = false;
    auto moveBody = [&](const float* cp) {   // controller (out of the world) and the entity the camera follows
        CharSetPos(c, cp); CharSetVel(c, 0, 0, 0);
        if (modeUsed != 2) return;
        float q[4]; CharGetRot(c, q);
        float yaw = 2.0f * atan2f(q[2], q[3]);
        xr::HandState hands[2]; XrPosef head; bool headValid = false;
        float sx = xr::InputSnapshot(hands,head,headValid) ? hands[1].stick.x : 0;
        bool push = fabsf(sx) > 0.35f && controls::SnapDegrees() < 0.5f;   // snap turning turns the body itself (SnapUpdate)
        if (push && !ownTurn) {
            float d = fabsf(yaw - yawPrev); if (d > 3.14159f) d = 6.28318f - d;
            stillFrames = d < 1e-4f ? stillFrames + 1 : 0;
            if (stillFrames > 6) { ownTurn = true; LOG("climb: the game doesn't turn the body on the wall - turning it here"); }
        }
        if (push && ownTurn) {
            float a = -(sx > 0 ? 1.0f : -1.0f) * g_c.climbTurnSpeed * 0.0174533f * fminf(fmaxf(dt, 0.0f), 0.05f);
            float h = 0.5f * a, zs = sinf(h), zc = cosf(h);   // world Z rotation, applied first: qz * q
            float n[4] = { zc * q[0] - zs * q[1], zc * q[1] + zs * q[0], zc * q[2] + zs * q[3], zc * q[3] - zs * q[2] };
            memcpy(q, n, 16); CharSetRot(c, q);
            yaw = 2.0f * atan2f(q[2], q[3]);
        }
        yawPrev = yaw;
        float e[3] = { cp[0] + footOfs[0], cp[1] + footOfs[1], cp[2] + footOfs[2] };
        EntSetPosRot(playerEnt, e, q, c);
        static int tl = 0; if (push && (++tl % 15) == 0) LOG("climb: turning (stick %.2f, yaw %.1f deg%s)", sx, yaw * 57.2958f, ownTurn ? ", ours" : "");
    };
    if (act < 0) {
        // no hand on a hold: hang where we are for a moment (ClimbGrace) so the hand can reach for the next hold
        if (g_climbWas && c && g_climbGrace > 0) {
            if (g_climbGraceT <= 0) { g_climbGraceT = g_climbGrace; LOG("climb: both hands free, hanging for %.1f s", g_climbGrace); }
            g_climbGraceT -= fmaxf(dt, 0.0f);
            if (g_climbGraceT > 0) {
                if (holdValid) { if (modeUsed == 2) moveBody(holdPos); else CharSetVel(c, 0, 0, 0); }
                return;
            }
        }
        if (g_climbWas && c) { if (modeUsed == 2) CharEnable(c, true); CharSetVel(c, 0, 0, 0); LOG("climb: let go"); if (!g_userHolster) HolsterLock(playerEnt, false); }
        g_climbWas = false; lastGen = 0; g_climbGraceT = 0; holdValid = false; return;
    }
    if (g_climbGraceT > 0) { LOG("climb: caught a new hold while hanging"); g_climbGraceT = 0; }
    if (!c) { for (auto& h : g_climb) h.on = false; return; }
    float p[3]; if (!CharGetPos(c, p)) return;
    const float* refNow = act ? f.gripO : f.lgripO;
    if (!g_climbWas) {
        g_climbWas = true; modeUsed = g_climbMode;
        float feet[3]; EntPos(playerEnt, feet); for (int k = 0; k < 3; ++k) footOfs[k] = feet[k] - p[k];
        if (modeUsed == 2) CharEnable(c, false);
        HolsterLock(playerEnt, true);
        LOG("climb: holding on (mode %d, controller state %d)", modeUsed, *(int*)(c + 0x178));
    }
    if (g_climb[act].gen != lastGen) {   // a new hold took over: start from where the body is now
        memcpy(bodyStart, p, 12); memcpy(g_climb[act].refStart, refNow, 12); lastGen = g_climb[act].gen;
    }
    float dref[3] = { g_climb[act].refStart[0] - refNow[0], g_climb[act].refStart[1] - refNow[1], g_climb[act].refStart[2] - refNow[2] }, dw[3];
    refDirToWorld(dref, dw);
    float target[3] = { bodyStart[0] + dw[0], bodyStart[1] + dw[1], bodyStart[2] + dw[2] };
    float step[3] = { target[0] - p[0], target[1] - p[1], target[2] - p[2] };
    float len = sqrtf(Dot3(step, step)), maxStep = 4.0f * fmaxf(dt, 0.005f);   // at most 4 m/s
    if (len > maxStep) for (float& x : step) x *= maxStep / len;
    static int frame = 0;
    if ((++frame % 10) == 0) { float e[3]; EntPos(playerEnt, e); LOG("climb: pos %.2f %.2f %.2f target %.2f %.2f %.2f entity z %.2f (hand moved %.2f %.2f %.2f in the room)", p[0], p[1], p[2], target[0], target[1], target[2], e[2], dref[0], dref[1], dref[2]); }
    // over the edge: the holding hand is below the chest (room space) -> climb onto what is in front of the hold
    if (f.headValid && refNow[1] < f.headO[1] - g_c.mantleBelowHead) {
        float fw[3]; refDirToWorld(f.headF, fw); fw[2] = 0; Normalize(fw);
        const float* a = g_climb[act].anchor;   // world point of the hold (taken when it was grabbed)
        float aw[3] = { a[0], a[1], a[2] };
        float probe[3] = { aw[0] + fw[0] * 0.45f, aw[1] + fw[1] * 0.45f, aw[2] + 1.2f }, down[3] = { 0, 0, -1 }, floor[3];
        if (GroundRay(probe, down, 1.6f, floor) && floor[2] > aw[2] - 0.4f) {
            float feet[3] = { p[0] + footOfs[0], p[1] + footOfs[1], p[2] + footOfs[2] };
            for (int k = 0; k < 3; ++k) p[k] += floor[k] - feet[k];
            p[2] += 0.05f;
            if (modeUsed == 2) CharEnable(c, true);
            CharSetPos(c, p); CharSetVel(c, 0, 0, 0);
            for (auto& h : g_climb) h.on = false;
            g_climbWas = false; lastGen = 0; holdValid = false; g_climbGraceT = 0;
            LOG("climb: pulled up over the edge onto %.2f %.2f %.2f", floor[0], floor[1], floor[2]);
            if (!g_userHolster) HolsterLock(playerEnt, false);
            return;
        }
    }
    if (modeUsed == 1) {
        float k = 1.0f / fmaxf(0.008f, dt);
        CharSetVel(c, step[0] * k, step[1] * k, step[2] * k);
    } else {
        for (int k = 0; k < 3; ++k) p[k] += step[k];
        moveBody(p);
    }
    memcpy(holdPos, p, 12); holdValid = true;
}


// ---------------------------------------------------------------- v231 room-scale walking
// The render thread publishes how far the real head is from the body (horizontal, game space). Past a short leash
// (leaning) the player's character controller is moved after the head, at most RoomScaleMaxSpeed, and stops
// RoomScaleWallDistance short of walls (rays at knee and chest height). The amount moved goes back to the render
// thread, which moves its recenter point by the same distance so the view stays where the head is.
static float g_rsOff[3] = {}; static DWORD g_rsTick = 0; static bool g_rsValid = false;
static float g_rsMoved[3] = {};
void SetRoomScaleOffset(const float off[3], bool valid) { Lock(); memcpy(g_rsOff, off, 12); g_rsValid = valid; g_rsTick = GetTickCount(); Unlock(); }
bool TakeRoomScaleMoved(float out[3]) {
    Lock(); memcpy(out, g_rsMoved, 12); g_rsMoved[0] = g_rsMoved[1] = g_rsMoved[2] = 0; Unlock();
    return out[0] != 0 || out[1] != 0 || out[2] != 0;
}
// free distance along a horizontal direction from p at height h above the feet (the player's own body ignored)
static float RoomScaleFree(const float* p, const float* d, float h, float len, uint8_t* playerEnt) {
    float from[3] = { p[0], p[1], p[2] + h };
    float travelled = 0;
    for (int tries = 0; tries < 3; ++tries) {
        float hit[3], n[3]; uint32_t key = 0;
        if (!GroundRayEx(from, d, len - travelled, hit, n, &key)) return len;
        float dist = Dist(hit, from);
        Handle* hh = HitEntity(key); uint8_t* ent = hh ? hh->ent : nullptr; HandleRelease(hh);
        if (ent != playerEnt) return travelled + dist;
        travelled += dist + 0.02f;   // the player's own body: look past it
        for (int k = 0; k < 3; ++k) from[k] = hit[k] + d[k] * 0.02f;
        if (travelled >= len) return len;
    }
    return len;
}
static void RoomScaleUpdate(uint8_t* playerEnt, float dt) {
    static int nLog = 0; static double logT = 0, movedSum = 0; static int blocked = 0;
    if (!g_c.roomScale || dt <= 0 || dt > 0.1f) return;
    float off[3]; bool valid; DWORD tick;
    Lock(); memcpy(off, g_rsOff, 12); valid = g_rsValid; tick = g_rsTick; Unlock();
    if (!valid || GetTickCount() - tick > 200) return;
    if (g_climbWas || controls::VehicleMode() != 0 || controls::MountedWeapon() || !controls::NormalControls()) return;
    uint8_t* c = PlayerCharComp(playerEnt); if (!c) return;
    float len = sqrtf(off[0] * off[0] + off[1] * off[1]);
    if (len <= g_c.roomLeash || !std::isfinite(len)) return;
    float d[3] = { off[0] / len, off[1] / len, 0 };
    float want = len - g_c.roomLeash, cap = g_c.roomMaxSpeed * dt;
    if (want > cap) want = cap;
    float p[3]; if (!CharGetPos(c, p)) return;
    float reach = want + g_c.roomRadius;
    float free = fminf(RoomScaleFree(p, d, 0.45f, reach, playerEnt), RoomScaleFree(p, d, 1.20f, reach, playerEnt));
    float step = fminf(want, fmaxf(0.0f, free - g_c.roomRadius));
    if (step < want - 0.001f) ++blocked;
    if (step < 0.0005f) return;
    float np[3] = { p[0] + d[0] * step, p[1] + d[1] * step, p[2] };
    CharSetPos(c, np);
    Lock(); g_rsMoved[0] += d[0] * step; g_rsMoved[1] += d[1] * step; Unlock();
    movedSum += step;
    double now = GetTickCount64() * 1.0;
    if (nLog < 40 && now - logT > 5000.0) { ++nLog; logT = now; LOG("room-scale: body followed the head %.2f m in the last 5 s (head %.2f m from the body, %d steps shortened by walls)", movedSum, len, blocked); movedSum = 0; blocked = 0; }
}

// ---------------------------------------------------------------- snap turning
// One push of the right stick turns the body by SnapTurn degrees at once: the character controller's orientation
// (+0x88 SetOrientation) is turned about the world up axis. If the game puts it back (vehicles, scripted states), the
// turn is done through the gamepad stick instead, steered by the controller's yaw until the angle is reached.
static float AngDiff(float a, float b) {   // a - b wrapped to (-pi, pi]; any finite input (fmod: never loops)
    float d = fmodf(a - b, 6.2831853f); if (!std::isfinite(d)) return 1e9f;
    if (d > 3.14159265f) d -= 6.2831853f; else if (d <= -3.14159265f) d += 6.2831853f; return d;
}
static uint8_t* volatile g_snapChar = nullptr;   // the player's character controller (for the diagnostic hook)
static volatile LONG g_snapWatch = 0;             // frames left to log who sets its orientation after a snap
typedef void (__fastcall *CharSetRot_t)(void* self, void* edx, const float* q);
static CharSetRot_t o_CharSetRot = nullptr;
static void __fastcall Hook_CharSetRot(void* self, void* edx, const float* q) {
    if (self == g_snapChar && g_snapWatch > 0 && q) {
        static uintptr_t seen[8]; static int ns = 0;
        uintptr_t raw = (uintptr_t)__builtin_return_address(0);
        if (raw < g_base || raw >= g_base + 0x1400000) { o_CharSetRot(self, edx, q); return; }   // not the game (the mod's own call)
        uintptr_t ra = raw - g_base + 0x10000000;
        bool known = false; for (int i = 0; i < ns; ++i) known |= seen[i] == ra;
        if (!known && ns < 8) { seen[ns++] = ra; LOG("snap: the game sets the player's orientation from %08x (yaw %.1f deg)", (unsigned)ra, 2.0f * atan2f(q[2], q[3]) * 57.2958f); }
    }
    o_CharSetRot(self, edx, q);
}
// diagnostic: where does the game keep the player's heading? A stick turn's memory before/after is compared for
// floats that changed by the turn angle (radians or degrees) and unit pairs (direction vectors, quaternion z/w)
// whose angle changed by it (or half of it).
struct SnapRegion { const char* name; uint8_t* p; int n; };
static float g_snapSnap[6][0x800 / 4]; static SnapRegion g_snapReg[6]; static int g_snapNReg = 0; static float g_snapY0 = 0;
static void SnapScanBegin(uint8_t* playerEnt, uint8_t* c, float yaw) {
    g_snapNReg = 0; g_snapY0 = yaw;
    uint8_t* pd = g_pawn ? *(uint8_t**)(g_pawn + 0x10) : nullptr;
    SnapRegion r[6] = { { "pawn", g_pawn, 0x600 }, { "pawndata", pd, 0x800 }, { "charcomp", c, 0x200 }, { "entity", playerEnt, 0x200 },
                        { "char+160", c ? *(uint8_t**)(c + 0x160) : nullptr, 0x400 }, { "char+164", c ? *(uint8_t**)(c + 0x164) : nullptr, 0x400 } };
    for (auto& x : r) {
        if (!x.p || IsBadReadPtr(x.p, x.n)) continue;
        g_snapReg[g_snapNReg] = x; memcpy(g_snapSnap[g_snapNReg], x.p, x.n); ++g_snapNReg;
    }
}
static void SnapScanEnd(float yaw) {
    static int runs = 0; if (runs >= 3) return; ++runs;
    float d = AngDiff(yaw, g_snapY0); int hits = 0;
    LOG("snap scan %d: yaw %.1f -> %.1f deg (turned %.1f)", runs, g_snapY0 * 57.2958f, yaw * 57.2958f, d * 57.2958f);
    for (int ri = 0; ri < g_snapNReg; ++ri) {
        SnapRegion& R = g_snapReg[ri]; if (IsBadReadPtr(R.p, R.n)) continue;
        const float* o = g_snapSnap[ri]; const float* nw = (const float*)R.p; int cnt = R.n / 4;
        for (int k = 0; k < cnt && hits < 40; ++k) {
            float a0 = o[k], a1 = nw[k];
            if (!std::isfinite(a0) || !std::isfinite(a1) || a0 == a1) continue;
            float dd = a1 - a0;
            bool rad = fabsf(AngDiff(dd, d)) < 0.03f || fabsf(AngDiff(dd, -d)) < 0.03f;
            bool deg = fabsf(dd - d * 57.2958f) < 1.5f || fabsf(dd + d * 57.2958f) < 1.5f || fabsf(fabsf(dd) - 360.0f + fabsf(d) * 57.2958f) < 1.5f;
            if ((rad && fabsf(a0) < 7.0f) || deg) { ++hits; LOG("  %s+%03x: %.4f -> %.4f (%s)", R.name, k * 4, a0, a1, rad ? "radians" : "degrees"); }
            if (k + 1 < cnt) {
                float b0 = o[k + 1], b1 = nw[k + 1];
                float l0 = a0 * a0 + b0 * b0, l1 = a1 * a1 + b1 * b1;
                if (std::isfinite(b0) && std::isfinite(b1) && fabsf(l0 - 1) < 0.02f && fabsf(l1 - 1) < 0.02f) {
                    float ang = AngDiff(atan2f(b1, a1), atan2f(b0, a0));
                    bool full = fabsf(fabsf(ang) - fabsf(d)) < 0.03f, half = fabsf(fabsf(ang) - fabsf(d) * 0.5f) < 0.02f;
                    if ((full || half) && fabsf(ang) > 0.05f) { ++hits; LOG("  %s+%03x/+%03x: (%.3f %.3f) -> (%.3f %.3f) pair turned %.1f deg%s", R.name, k * 4, k * 4 + 4, a0, b0, a1, b1, ang * 57.2958f, half ? " (half: quaternion)" : ""); }
                }
            }
        }
    }
    LOG("snap scan %d: %d candidates", runs, hits);
}
static void SnapUpdate(uint8_t* playerEnt, float dt) {
    static int verify = 0, fails = 0, queued = 0; static float target = 0, burstT = 0; static bool burst = false; static int nLog = 0;
    float deg = controls::SnapDegrees();
    uint8_t* c = PlayerCharComp(playerEnt);
    g_snapChar = c;
    if (g_snapWatch > 0) InterlockedDecrement(&g_snapWatch);
    if (deg < 0.5f && c) {   // v224 smooth turning faster than the game's own: the extra yaw each frame, as the snap turn does
        float ex = controls::TurnExtra();
        if (ex != 0.0f && dt > 0 && dt < 0.1f) {
            float a = -ex * 2.618f * dt;   // the game's full-stick rate ~150 deg/s; stick right turns the yaw down
            float q[4]; CharGetRot(c, q);
            float h = 0.5f * a, zs = sinf(h), zc = cosf(h);
            float n[4] = { zc * q[0] - zs * q[1], zc * q[1] + zs * q[0], zc * q[2] + zs * q[3], zc * q[3] - zs * q[2] };
            CharSetRot(c, n);
            uint8_t* pd = g_pawn ? *(uint8_t**)(g_pawn + 0x10) : nullptr;
            if (pd && !IsBadWritePtr(pd + 0x180, 0x194)) {
                static const int offs[3] = { 0x180, 0x18C, 0x310 };
                for (int o : offs) { float* v = (float*)(pd + o); if (std::isfinite(*v) && fabsf(*v) < 7.0f) *v = AngDiff(*v + a, 0.0f); }
            }
        }
    }
    if (deg < 0.5f || !c) { controls::SetSnapStick(0); burst = false; verify = 0; queued = 0; controls::TakeSnap(); return; }
    float q[4]; CharGetRot(c, q);
    float yaw = 2.0f * atan2f(q[2], q[3]);
    queued += controls::TakeSnap();
    if (queued > 2) queued = 2; if (queued < -2) queued = -2;
    if (verify > 0 && --verify == 0) {
        float off = AngDiff(yaw, target);
        if (fabsf(off) > 0.17f) {   // put back by the game: let the game turn it
            ++fails; burst = true; burstT = 0;
            if (nLog++ < 20) LOG("snap: orientation not kept (%.0f deg off) - turning through the stick%s", off * 57.2958f, fails >= 3 ? " (from now on)" : "");
        } else { fails = 0; if (nLog++ < 6) LOG("snap: turned to %.1f deg", yaw * 57.2958f); }
    }
    if (burst) {   // the game turns at full stick until the angle is reached (no easing: below ~0.4 the stick is in its dead zone)
        float rem = AngDiff(target, yaw); burstT += dt;
        static float remPrev = 0; bool crossed = burstT > dt * 1.5f && remPrev * rem < 0;
        remPrev = rem;
        if (fabsf(rem) < 0.05f || crossed || burstT > 1.0f) {
            burst = false; controls::SetSnapStick(0);
            if (nLog++ < 30) LOG("snap: stick turn done (%.1f deg left, %.2f s)", rem * 57.2958f, burstT);
        } else controls::SetSnapStick(rem < 0 ? 1.0f : -1.0f);   // stick right turns the yaw down
        return;
    }
    if (verify > 0 || !queued) return;
    int r = queued > 0 ? 1 : -1; queued -= r;
    float a = -(float)r * deg * 0.0174533f;
    target = yaw + a;
    if (fails >= 3) { burst = true; burstT = 0; return; }
    float h = 0.5f * a, zs = sinf(h), zc = cosf(h);   // world Z rotation, applied first: qz * q
    float n[4] = { zc * q[0] - zs * q[1], zc * q[1] + zs * q[0], zc * q[2] + zs * q[3], zc * q[3] - zs * q[2] };
    CharSetRot(c, n);
    // the game's own heading (radians, found by a memory scan): pawn data +0x180 / +0x18C (DesiredData +0x40/+0x4C)
    // and +0x310 (EffectiveData +0x40); the controller's orientation follows them
    uint8_t* pd = g_pawn ? *(uint8_t**)(g_pawn + 0x10) : nullptr;
    if (pd && !IsBadWritePtr(pd + 0x180, 0x194)) {
        static const int offs[3] = { 0x180, 0x18C, 0x310 };
        for (int o : offs) {
            float* v = (float*)(pd + o);
            if (std::isfinite(*v) && fabsf(*v) < 7.0f) *v = AngDiff(*v + a, 0.0f);   // turned and wrapped to (-pi, pi]
        }
    }
    InterlockedExchange(&g_snapWatch, 4);
    verify = 3;
}

// ---------------------------------------------------------------- safehouse watch (sleep): the right stick sets the time
// CGadgetUseWatchStrategy update (0x1072DFD0, thiscall this, float dt): in its time-setting mode (+0x80 == 1) the
// time hand turns by the input axis at +0x68 (written by the input listener from A / D); the right stick writes it.
typedef void (__fastcall *WatchUpd_t)(void* self, void* edx, float dt);
static WatchUpd_t o_WatchUpd = nullptr;
static void __fastcall Hook_WatchUpd(void* self, void* edx, float dt) {
    uint8_t* s = (uint8_t*)self;
    static bool wrote = false; static int nLog = 0;
    if (*(int*)(s + 0x80) == 1) {
        controls::SetWatchActive();
        float sx = xr::Hand(1).stick.x;
        if (fabsf(sx) > 0.3f) { *(float*)(s + 0x68) = (sx > 0 ? 1.0f : -1.0f) * g_c.watchStickSign; wrote = true; if (nLog < 4 && !nLog++) LOG("watch: right stick sets the time"); }
        else if (wrote) { *(float*)(s + 0x68) = 0; wrote = false; }
    }
    o_WatchUpd(self, edx, dt);
}

// ---- throwing arc (grip + trigger of a hand): the arc follows the controller, the throw lands where it ends.
// CGadgetUseThrowStrategy::ComputeThrowParams 0x101AA3A0 (thiscall strategy, ctx; [ctx+0x10] = thrower's pawn data)
// writes the launch position (+0xA4) and velocity (+0xB0, world m/s) that Launch (0x101AB870) then gives the
// projectile; fThrowStrength at +0x64. After it runs, a pending player throw replaces both.
static const float kThrowG = 9.82f;
static float g_arcRef[64][3], g_arcNrmRef[3]; static int g_arcN = 0; static bool g_arcLand = false, g_arcOk = false;
static float g_arcPos[3], g_arcVel[3]; static bool g_arcValid = false;
static fc2vr::authority::Stamp g_arcSource; static int g_arcHand = -1;
static bool g_throwPend = false; static float g_pendPos[3], g_pendVel[3]; static DWORD g_pendT = 0;
static fc2vr::authority::Stamp g_pendingSource; static int g_pendingHand = -1; static uint8_t* g_pendingPd = nullptr;
static float g_throwStrength = 0;   // learned from the game at the first throw
static uint8_t* g_throwPd = nullptr;
typedef void (__fastcall *ThrowCalc_t)(void* strat, void* edx, void* ctx);
static ThrowCalc_t o_ThrowCalc = nullptr;
static void __fastcall Hook_ThrowCalc(void* strat, void* edx, void* ctx) {
    o_ThrowCalc(strat, edx, ctx);
    uint8_t* st = (uint8_t*)strat;
    uint8_t* pd = ctx && !IsBadReadPtr(ctx, 0x14) ? *(uint8_t**)((uint8_t*)ctx + 0x10) : nullptr;
    if (!st || !pd || pd != g_throwPd) return;
    float str = *(float*)(st + 0x64);
    if (str > 0.5f && str < 200.0f && fabsf(str - g_throwStrength) > 0.01f) { g_throwStrength = str; LOG("throw: the game's throw strength is %.1f m/s", str); }
    float pos[3],vel[3]; fc2vr::authority::Stamp source; int hand;
    Lock(); const bool pending = g_throwPend && pd == g_pendingPd && GetTickCount()-g_pendT < 3000;
    source = g_pendingSource; hand = g_pendingHand; memcpy(pos,g_pendPos,12); memcpy(vel,g_pendVel,12); g_throwPend = false; Unlock();
    if (pending && xr::AcceptedInputCurrent(source,hand)) {
        float* gp = (float*)(st + 0xA4); float* gv = (float*)(st + 0xB0);
        LOG("throw: arc launch from %.2f %.2f %.2f vel %.2f %.2f %.2f (game: from %.2f %.2f %.2f vel %.2f %.2f %.2f)",
            pos[0],pos[1],pos[2],vel[0],vel[1],vel[2],gp[0],gp[1],gp[2],gv[0],gv[1],gv[2]);
        memcpy(gp,pos,12); memcpy(gv,vel,12);
    }
}

// ---- test helper: grenades / molotovs to the maximum. Throwables are CGadget items in the inventory pack
// ([[pd+0x4F0+4]+4] = {item**, count}); 0x1018FD00 (thiscall item) -> CGadget* or 0; AddAmmo 0x10149EC0 (thiscall g, n)
// caps at the gadget's max. Missing ones are spawned (0x109FAFF0 cdecl(&id, archetype)) and given to the player
// (0x109F9460 cdecl(id)), as the game's script functions SpawnWeapon / AddGadget do.
static volatile LONG g_giveThrow = 0;
void RequestGiveThrowables() { InterlockedExchange(&g_giveThrow, 1); }
static int FillGadgets(uint8_t* pd, bool* haveGren, bool* haveMolo) {
    uint8_t* inner = *(uint8_t**)(pd + 0x4F0 + 4);
    if (!inner || IsBadReadPtr(inner, 0x30)) return 0;
    struct Pack { void** items; uint32_t count; };
    Pack* pk = (Pack*)(inner + 4);
    if (!pk->items || pk->count > 256 || IsBadReadPtr(pk->items, pk->count * 4)) return 0;
    typedef void* (__fastcall *ToGadget_t)(void* item, void* edx);
    typedef int (__fastcall *AddAmmo_t)(void* g, void* edx, int n);
    typedef int (__fastcall *GetInt_t)(void* g, void* edx);
    int n = 0;
    for (uint32_t i = 0; i < pk->count; ++i) {
        void* it = pk->items[i]; if (!it || IsBadReadPtr(it, 0x10)) continue;
        void* g = ((ToGadget_t)A(0x1018FD00))(it, nullptr);
        if (!g || IsBadReadPtr(g, 0x28)) continue;
        // name of the gadget's entity: [[g+8]+0xC] -> entity, std::string at +0x14 (MSVC: inline buffer if cap < 16)
        char name[64] = "?";
        uint8_t* h = *(uint8_t**)((uint8_t*)g + 8);
        uint8_t* ent = h && !IsBadReadPtr(h, 0x10) ? *(uint8_t**)(h + 0xC) : nullptr;
        if (ent && !IsBadReadPtr(ent + 0x14, 0x1C)) {
            uint32_t len = *(uint32_t*)(ent + 0x14 + 0x10), cap = *(uint32_t*)(ent + 0x14 + 0x14);
            const char* str = cap < 16 ? (const char*)(ent + 0x14) : *(const char**)(ent + 0x14);
            if (len < 63 && str && !IsBadReadPtr(str, len)) { memcpy(name, str, len); name[len] = 0; }
        }
        int before = ((GetInt_t)A(0x101496C0))(g, nullptr);
        int added = ((AddAmmo_t)A(0x10149EC0))(g, nullptr, 999);
        int maxA = ((GetInt_t)A(0x10149E30))(g, nullptr);
        LOG("throwables: gadget %s: %d -> %d (max %d)", name, before, before + added, maxA);
        if (strstr(name, "olotov") || strstr(name, "OLOTOV")) { if (haveMolo) *haveMolo = true; }
        else if (haveGren) *haveGren = true;
        ++n;
    }
    return n;
}
static void GiveThrowables(uint8_t* pd) {
    if (!pd) return;
    bool gren = false, molo = false;
    int n = FillGadgets(pd, &gren, &molo);
    typedef void* (__cdecl *Spawn_t)(uint64_t* out, const char* arch);
    typedef void (__cdecl *AddGadget_t)(uint64_t id);
    const char* want[2] = { gren ? nullptr : "gadgets.Grenades.M67", molo ? nullptr : "gadgets.Grenades.Molotov" };
    bool added = false;
    for (const char* a : want) {
        if (!a) continue;
        uint64_t id = 0;
        ((Spawn_t)A(0x109FAFF0))(&id, a);
        LOG("throwables: spawned %s -> id %08x%08x", a, (unsigned)(id >> 32), (unsigned)id);
        if (id == 0 || id == ~0ull || (uint32_t)id == 0xFFFFFFFFu) continue;
        ((AddGadget_t)A(0x109F9460))(id);
        added = true;
    }
    if (added) n = FillGadgets(pd, nullptr, nullptr);
    typedef void* (__fastcall *Q_t)(void* inv);
    typedef bool (__fastcall *Cycle_t)(void* inv);
    if (!((Q_t)A(0x10129EC0))(pd + 0x4F0)) ((Cycle_t)A(0x10129F10))(pd + 0x4F0);
    LOG("throwables: %d gadget(s), throwable ready %d", n, ((Q_t)A(0x10129EC0))(pd + 0x4F0) != nullptr);
}
template <class TW, class TR>
static void ThrowArcUpdate(const Frame& f, uint8_t* pd, TW toWorld, TR toRef) {
    g_throwPd = pd;
    if (InterlockedExchange(&g_giveThrow, 0)) GiveThrowables(pd);
    const int hand = controls::ThrowAimHand();
    static int lastHand = 1; if (hand >= 0) lastHand = hand;
    const bool release = controls::TakeThrowRelease() != 0;
    typedef void* (__fastcall *Q_t)(void* inv);
    void* throwable = pd ? ((Q_t)A(0x10129EC0))(pd + 0x4F0) : nullptr;
    if (release) {
        const bool tracked = g_arcHand == 1 ? f.handValid : g_arcHand == 0 ? f.leftValid : false;
        if (g_arcValid && throwable && tracked && xr::InputCurrent(g_arcSource,g_arcHand)) {
            Lock(); memcpy(g_pendPos,g_arcPos,12); memcpy(g_pendVel,g_arcVel,12); g_pendT = GetTickCount(); g_throwPend = true;
            g_pendingSource = g_arcSource; g_pendingHand = g_arcHand; g_pendingPd = pd; Unlock();
            controls::FireThrow();
            xr::Haptic(lastHand, 0.6f, 0.08f);
            LOG("throw: released - throwing along the arc");
        } else LOG("throw: released without %s", throwable ? "an arc" : "a grenade or molotov");
    }
    int n = 0; bool land = false; float nrm[3] = { 0, 0, 1 };
    g_arcValid = false;
    const bool valid = hand == 1 ? f.handValid : hand == 0 ? f.leftValid : false;
    if (hand >= 0 && valid) {
        float o[3], b[3], d[3];
        toWorld(hand ? f.aimO : f.laimO, o, true); toWorld(hand ? f.aimB : f.laimB, b, false);
        for (int k = 0; k < 3; ++k) d[k] = -b[k];
        Normalize(d);
        const float speed = (g_c.throwSpeed > 0.5f ? g_c.throwSpeed : g_throwStrength > 0.5f ? g_throwStrength : 15.0f) * sqrtf(fmaxf(0.1f, g_c.throwRange));
        float v[3] = { d[0] * speed, d[1] * speed, d[2] * speed };
        memcpy(g_arcPos,o,12); memcpy(g_arcVel,v,12); g_arcValid = true; g_arcSource = f.source; g_arcHand = hand;
        float p[3]; memcpy(p, o, 12); float pv[3]; memcpy(pv, v, 12);
        float pts[64][3]; memcpy(pts[n++], p, 12);
        const float dt = 1.0f / 20.0f;
        for (int i = 1; i <= 100 && n < 64; ++i) {   // up to 5 s of flight
            float q[3] = { p[0] + pv[0] * dt, p[1] + pv[1] * dt, p[2] + pv[2] * dt - 0.5f * kThrowG * dt * dt };
            pv[2] -= kThrowG * dt;
            float seg[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] }; float len = sqrtf(Dot3(seg, seg));
            if (i > 2 && len > 1e-4f) {   // skip the first ~0.1 s (the player's own body)
                float dn[3] = { seg[0] / len, seg[1] / len, seg[2] / len }, hit[3], hn[3];
                if (GroundRayEx(p, dn, len, hit, hn, nullptr)) { memcpy(pts[n++], hit, 12); memcpy(nrm, hn, 12); land = true; break; }
            }
            memcpy(p, q, 12);
            if (i % 2 == 0 || i == 100) memcpy(pts[n++], p, 12);
        }
        float ref[64][3]; for (int i = 0; i < n; ++i) toRef(pts[i], ref[i]);
        float nr[3]; Put3(nr, f.game.ReferenceDirection(Read3(nrm)));
        Lock(); memcpy(g_arcRef, ref, sizeof(float) * 3 * n); g_arcN = n; g_arcLand = land; memcpy(g_arcNrmRef, nr, 12); g_arcOk = throwable != nullptr; Unlock();
        static int lg = 0; if (lg < 3 && land) { ++lg; LOG("throw: arc lands %.1f m away (%d points, speed %.1f)", Dist(pts[n - 1], o), n, speed); }
    } else { Lock(); g_arcN = 0; Unlock(); }
}
int GetThrowArc(float (*pts)[3], int maxPts, bool& land, float normal[3], bool& ok) {
    if (!g_csInit) return 0;
    Lock(); int n = g_arcN < maxPts ? g_arcN : maxPts; memcpy(pts, g_arcRef, sizeof(float) * 3 * n); land = g_arcLand; memcpy(normal, g_arcNrmRef, 12); ok = g_arcOk; Unlock();
    return n;
}

// ---- contact pushes: a moving hand (or the gun / machete in it) shoves loose objects and bodies it touches. Points
// along the hand (and the weapon's line) are tested against nearby dynamic bodies; a body the hand moves into gets the
// hand's speed along that direction (lighter objects fully, heavy ones less). Corpse ragdoll parts are pushed the same way.
template <class TW>
static void ContactPush(const Frame& f, uint8_t* playerEnt, TW toWorld, void* weapon) {
    static double lastLog = 0;
    for (int hnd = 0; hnd < 2; ++hnd) {
        bool valid = hnd ? f.handValid : f.leftValid;
        if (!valid || g_held[hnd].active || g_npc[hnd].on || g_climb[hnd].on) continue;
        float hv[3]; if (!HistVel(g_hist[hnd], 0.05, hv)) continue;
        float sp = sqrtf(Dot3(hv, hv)); if (sp < g_c.pushMinSpeed) continue;
        float n[3] = { hv[0] / sp, hv[1] / sp, hv[2] / sp };
        float hp[3], hb[3]; toWorld(hnd ? f.gripO : f.lgripO, hp, true); toWorld(hnd ? f.aimB : f.laimB, hb, false);
        // contact points: the hand, and along the held weapon (right hand: the shot line; machete: the hand's forward)
        float pts[4][3]; int np = 0; memcpy(pts[np++], hp, 12);
        bool armed = hnd == 1 && weapon;
        if (armed) {
            float d[3] = { -hb[0], -hb[1], -hb[2] }; Normalize(d);
            float len = g_c.pushWeaponLen;
            for (int k = 1; k <= 3; ++k) { for (int j = 0; j < 3; ++j) pts[np][j] = hp[j] + d[j] * len * k / 3.0f; ++np; }
        }
        for (int pi = 0; pi < np; ++pi) {
            Handle* hs[24]; int nh = OverlapHandles(pts[pi], 0.25f, hs, 24, 0xFFFFFFFFu);
            for (int i = 0; i < nh; ++i) {
                uint8_t* ent = hs[i]->ent;
                bool dup = false; for (int j = 0; j < i; ++j) if (hs[j]->ent == ent) dup = true;
                if (!ent || ent == playerEnt || dup) continue;
                // loose objects
                Rigid rg;
                if (GetRigid(ent, rg) && RigidDynamic(rg) && rg.box && BoxDist(rg, pts[pi]) < g_c.pushRadius) {
                    float k = fminf(1.0f, g_c.pushMass / fmaxf(1.0f, rg.mass));
                    for (int b = 0; b < rg.n; ++b) {
                        uint8_t* rb = rg.rb[b]; const float* bv = (float*)(rb + 0x1A0);
                        float along = Dot3(bv, n), want = sp * g_c.pushScale * k;
                        if (along < want) { float v[3] = { bv[0] + n[0] * (want - along), bv[1] + n[1] * (want - along), bv[2] + n[2] * (want - along) }; RbSetVel(rb, v, false); }
                    }
                    if (NowSec() - lastLog > 0.5) { lastLog = NowSec(); LOG("push: %s %s shoved an object (%.1f kg) at %.1f m/s", hnd ? "right" : "left", pi ? "weapon" : "hand", rg.mass, sp * g_c.pushScale * k); }
                    continue;
                }
                // bodies (ragdolls that are in the world: corpses, knocked-down NPCs)
                typedef uint8_t* (__fastcall *CharPhys_t)(void* ent, void* edx);
                uint8_t* comp = ((CharPhys_t)A(0x1007F3E0))(ent, nullptr);
                uint8_t** bb; int c;
                if (comp && *(uintptr_t*)comp == A(0x10E72FE0) && NpcDriver(comp) == 1 && RagBodies(comp, &bb, &c)) {
                    for (int b = 0; b < c; ++b) {
                        uint8_t* rb = bb[b]; if (!rb || IsBadReadPtr(rb, 0x1C0)) continue;
                        if (Dist((float*)(rb + 0x110), pts[pi]) > g_c.pushRadius + 0.12f) continue;
                        const float* bv = (float*)(rb + 0x1A0);
                        float along = Dot3(bv, n), want = sp * g_c.pushScale;
                        if (along < want) { float v[3] = { bv[0] + n[0] * (want - along), bv[1] + n[1] * (want - along), bv[2] + n[2] * (want - along) }; RbSetVel(rb, v, false); }
                    }
                }
            }
            for (int i = 0; i < nh; ++i) HandleRelease(hs[i]);
        }
    }
}

// ---- machete: a real swing of the blade into an NPC lands the game's own machete hit. The weapon's
// CWeaponFireMeleeStrategy ([[weapon+0x24]+0x54], vtbl 0x10E21DE0) supports an explicit target (as its network
// replay does): 0x1014EEA0 builds the target descriptor (thiscall desc, bone -1, id lo, id hi, &target pos,
// &origin offset from the attacker, flag; ret 0x18), 0x1012F130 (thiscall weapon, desc, 0; ret 8) sets it,
// DoMeleeHit 0x101550F0 (fastcall strategy) runs the stock hit - 1.5 m ray, damage stim, blood - and 0x1012E9D0
// (fastcall weapon) clears the target again.
struct MeleeHitLog { uint8_t* ent; double t; };
static MeleeHitLog g_meleeRecent[8];
template <class TW>
static void MacheteSwing(const Frame& f, uint8_t* playerEnt, TW toWorld, void* weapon) {
    if (!g_c.macheteHits || !weapon || !f.handValid || !controls::MacheteSelected() || !playerEnt) return;
    uint8_t* w = (uint8_t*)weapon;
    if (IsBadReadPtr(w + 0x24, 4)) return;
    uint8_t* a = *(uint8_t**)(w + 0x24);
    uint8_t* strat = a && !IsBadReadPtr(a + 0x54, 4) ? *(uint8_t**)(a + 0x54) : nullptr;
    if (!strat || IsBadReadPtr(strat, 0x40) || *(uintptr_t*)strat != A(0x10E21DE0)) {
        static int lg = 0; if (lg < 3) { ++lg; LOG("machete: weapon %p has no melee strategy (%p)", weapon, (void*)strat); }
        return;
    }
    // blade: from the hand along the controller's pointing direction
    float hp[3], hb[3]; toWorld(f.gripO, hp, true); toWorld(f.aimB, hb, false);
    float d[3] = { -hb[0], -hb[1], -hb[2] }; Normalize(d);
    float tip[3] = { hp[0] + d[0] * g_c.bladeLen, hp[1] + d[1] * g_c.bladeLen, hp[2] + d[2] * g_c.bladeLen };
    static float prevTip[3], prevRel[3]; static bool havePrev = false; static double tPrev = 0;
    double t = NowSec();
    float tipV = 0, relV = 0;
    float body[3]; EntPos(playerEnt, body);
    float rel[3] = { tip[0] - body[0], tip[1] - body[1], tip[2] - body[2] };   // the blade relative to the player (walking doesn't count as a swing)
    if (havePrev && t > tPrev) { tipV = Dist(tip, prevTip) / (float)(t - tPrev); relV = Dist(rel, prevRel) / (float)(t - tPrev); }
    memcpy(prevTip, tip, 12); memcpy(prevRel, rel, 12); havePrev = true; tPrev = t;
    {   // the game's machete swoosh (the two sounds its own attack plays, 2D on channel 15) on a fast swing
        static bool fast = false; static double lastSnd = 0;
        if (g_c.macheteSound && relV >= g_c.macheteSoundSpeed && !fast && t - lastSnd > 0.35) {
            fast = true; lastSnd = t;
            uintptr_t mp = A(0x11648F04); uint8_t* mgr = mp ? *(uint8_t**)mp : nullptr;
            if (mgr && !IsBadReadPtr(mgr, 4)) {
                typedef int (__fastcall *Play2D_t)(void* mgr, void* edx, uint32_t id, int channel, int z, float f);
                Play2D_t play = (Play2D_t)(*(void***)mgr)[0x9C / 4];
                for (uint32_t id : g_c.macheteSnd) if (id) play(mgr, nullptr, id, 15, 0, 0.0f);
                static int nl = 0; if (nl++ < 5) LOG("machete: swing sound at %.1f m/s (blade relative to the body)", relV);
            }
        }
        if (relV < g_c.macheteSoundSpeed * 0.5f) fast = false;
    }
    if (tipV < g_c.macheteSpeed) return;
    // an NPC the blade is in: standing pawns as an upright capsule, bodies by their ragdoll parts
    float mid[3] = { (hp[0] + tip[0]) * 0.5f, (hp[1] + tip[1]) * 0.5f, (hp[2] + tip[2]) * 0.5f };
    Handle* hs[24]; int nh = OverlapHandles(mid, g_c.bladeLen * 0.5f + 0.4f, hs, 24, 0xFFFFFFFFu);
    typedef uint8_t* (__fastcall *CharPhys_t)(void* ent, void* edx);
    uint8_t* hitEnt = nullptr; float hitP[3] = { 0, 0, 0 };
    for (int i = 0; i < nh && !hitEnt; ++i) {
        uint8_t* ent = hs[i]->ent;
        if (!ent || ent == playerEnt) continue;
        uint8_t* comp = ((CharPhys_t)A(0x1007F3E0))(ent, nullptr);
        if (!comp || *(uintptr_t*)comp != A(0x10E72FE0)) continue;
        bool recent = false; for (auto& r : g_meleeRecent) if (r.ent == ent && t - r.t < g_c.macheteCooldown) recent = true;
        if (recent) continue;
        float ep[3]; EntPos(ent, ep);
        for (int k = 0; k <= 4 && !hitEnt; ++k) {   // points along the blade
            float q[3]; for (int j = 0; j < 3; ++j) q[j] = hp[j] + (tip[j] - hp[j]) * (0.2f + 0.2f * k);
            float z = q[2] < ep[2] ? ep[2] : q[2] > ep[2] + 1.85f ? ep[2] + 1.85f : q[2];
            float dx = q[0] - ep[0], dy = q[1] - ep[1], dz = q[2] - z;
            if (dx * dx + dy * dy + dz * dz < 0.32f * 0.32f) { hitEnt = ent; memcpy(hitP, q, 12); }
        }
    }
    if (hitEnt) {
        float pp[3]; EntPos(playerEnt, pp);
        // the hit's ray (1.5 m) starts half a metre before the contact, on the line from the player's chest
        float n[3] = { hitP[0] - pp[0], hitP[1] - pp[1], hitP[2] - (pp[2] + 1.3f) }; Normalize(n);
        float o[3] = { hitP[0] - n[0] * 0.5f, hitP[1] - n[1] * 0.5f, hitP[2] - n[2] * 0.5f };
        float off[3] = { o[0] - pp[0], o[1] - pp[1], o[2] - pp[2] };
        uint32_t idLo = *(uint32_t*)(hitEnt + 8), idHi = *(uint32_t*)(hitEnt + 0xC);
        alignas(16) uint8_t desc[0x40] = {};
        alignas(16) float tp[4] = { hitP[0], hitP[1], hitP[2], 0 }, of[4] = { off[0], off[1], off[2], 0 };
        typedef void (__fastcall *Desc_t)(void* desc, void* edx, int bone, uint32_t lo, uint32_t hi, const float* pos, const float* off, int flag);
        typedef void (__fastcall *SetT_t)(void* weapon, void* edx, void* desc, int z);
        typedef void (__fastcall *Hit_t)(void* strat, void* edx);
        typedef void (__fastcall *Clr_t)(void* weapon, void* edx);
        ((Desc_t)A(0x1014EEA0))(desc, nullptr, -1, idLo, idHi, tp, of, 0);
        ((SetT_t)A(0x1012F130))(weapon, nullptr, desc, 0);
        ((Hit_t)A(0x101550F0))(strat, nullptr);
        ((Clr_t)A(0x1012E9D0))(weapon, nullptr);
        int slot = 0; double oldest = 1e30; for (int i = 0; i < 8; ++i) if (g_meleeRecent[i].t < oldest) { oldest = g_meleeRecent[i].t; slot = i; }
        g_meleeRecent[slot] = { hitEnt, t };
        xr::Haptic(1, 0.9f, 0.1f);
        LOG("machete: swing hit entity %p at %.1f m/s", (void*)hitEnt, tipV);
    }
    for (int i = 0; i < nh; ++i) HandleRelease(hs[i]);
}
static void HandInteract(uint8_t* pawn, uint8_t* pd) {
    if (!g_c.handGrab) return;
    Frame f; Lock(); f = g_frame; Unlock();
    if (!f.valid || g_camBoneSeq == 0) return;
    if (!xr::InputCurrent()) controls::CancelPendingInput();
    // Keep presentation fallback out of gameplay, including when render publication stops.
    f.handValid = f.handValid && f.handAuthority && xr::InputCurrent(f.source,1);
    f.leftValid = f.leftValid && f.leftAuthority && xr::InputCurrent(f.source,0);
    f.headValid = f.headValid && f.headAuthority && xr::HeadCurrent(f.source);
    static fc2vr::authority::Stamp previousInput;
    for (int hnd = 0; hnd < 2; ++hnd) {
        if (previousInput.generation != f.source.generation || previousInput.handGeneration[hnd] != f.source.handGeneration[hnd]) {
            // A short dropout can occur entirely between game ticks. Cancel old
            // holds/history once even if the newest sample is tracked again.
            if (hnd) f.handValid = false; else f.leftValid = false;
            g_hist[hnd].n = 0; g_gripPrev[hnd] = true;
            Lock(); g_npcDrive[hnd] = NpcDrive{}; Unlock();
        }
    }
    previousInput = f.source;
    f.twoHand = f.twoHand && f.handValid && f.leftValid;
    Lock(); g_frameUsage.haveInteraction = true; g_frameUsage.interactionSequence = f.frameSequence;
    ++g_frameUsage.interactionCalls; g_frameUsage.interactionThread = GetCurrentThreadId(); Unlock();
    auto toWorld = [&](const float* ref, float* o, bool point) { Put3(o, point ? f.game.Position(Read3(ref)) : f.game.Direction(Read3(ref))); };
    Handle* player = *(Handle**)(pawn + 8);
    g_pawn = pawn;
    uint8_t* playerEnt = player ? player->ent : nullptr;
    g_playerEntR = playerEnt;
    ReloadInputTick();
    {   // the held weapon's graphic component, as the projectile code finds it (entity -> component 0x104E2CC0 with
        // the type id at 0x10FDF1AC; muzzle bones are read from it with 0x1051D2F0, world matrix at +0x170)
        uint8_t* gfx = nullptr; uint8_t* sk = nullptr;
        void* w = g_curWeapon;
        uint8_t* wh = w && !IsBadReadPtr((uint8_t*)w + 8, 4) ? *(uint8_t**)((uint8_t*)w + 8) : nullptr;
        uint8_t* went = wh && !IsBadReadPtr(wh, 16) ? *(uint8_t**)(wh + 12) : nullptr;
        if (went && *(uint32_t*)A(0x10FDF194) != 0) {
            typedef uint8_t* (__fastcall *Comp_t)(void* ent, void* edx, const void* type);
            gfx = ((Comp_t)A(0x104E2CC0))(went, nullptr, (const void*)A(0x10FDF1AC));
            if (gfx && !IsBadReadPtr(gfx, 0x1B0)) { typedef uint8_t* (__fastcall *Sk_t)(void* g, void* edx); sk = ((Sk_t)(*(void***)gfx)[0x78 / 4])(gfx, nullptr); }
            else gfx = nullptr;
        }
        if (gfx != g_wGfx) { int sn = sk && !IsBadReadPtr(sk + 0x2C, 4) ? *(int*)(sk + 0x2C) : -1; LOG("arms: held weapon graphic %p, skeleton %p (%d bones)", gfx, sk, sn); }
        g_wGfx = gfx; g_wSkel = sk;
        HeldWeaponGfx(nullptr);   // game thread, each frame
        CallerReport();
        AttachWatch();
        MonoWatch(); FlashTick();
    }
    typedef void* (__fastcall *CurWeap_t)(void* inv);
    void* weapon = ((CurWeap_t)A(0x10127DA0))(pd + 0x4F0);
    g_curWeapon = weapon;
    {   // v209: the weapon's hold key, from its model: bone count, bone name hashes and bone lengths (5 cm steps). The HUD
        // reticle id was shared by most guns (one saved hold for a rifle, a pistol and a sniper rifle) and lags a weapon
        // change by seconds; the inventory slot wasn't found. The model is the weapon's own and is there at once.
        static void* kw = nullptr; static ULONGLONG since = 0;
        ULONGLONG tk = GetTickCount64();
        if (weapon != kw) { kw = weapon; since = tk; g_typeKey = 0; }
        if (weapon && !g_typeKey && tk - since > 300 && !ReloadActive()) {
            uint8_t* sk = g_wSkel; int nb = (sk && !IsBadReadPtr(sk + 0x2C, 4)) ? *(int*)(sk + 0x2C) : 0;
            Bone* bb = (sk && nb > 0 && nb <= 64) ? *(Bone**)(sk + 0x20) : nullptr;
            if (bb && !IsBadReadPtr(bb, nb * sizeof(Bone))) {
                uint32_t k = 2166136261u ^ (uint32_t)nb;
                char lens[160]; int lo = 0;
                for (int i = 0; i < nb; ++i) {
                    const float* L = bb[i].local; float len = sqrtf(L[12] * L[12] + L[13] * L[13] + L[14] * L[14]);
                    uint32_t q = (i == 0 || bb[i].parent < 0) ? 0u : (uint32_t)lroundf(len * 20.0f);   // 5 cm steps (the root's offset animates: left out)
                    k = (k ^ bb[i].hash) * 16777619u; k = (k ^ q) * 16777619u;
                    if (lo < 140) lo += snprintf(lens + lo, sizeof lens - lo, "%s%.2f", i ? " " : "", len);
                }
                if (!k || k == 0xFFFFFFFFu) k = 0x5EA70001u;
                g_typeKey = k;
                LOG("arms: weapon %p hold key %08x (%d bones, lengths %s)", weapon, k, nb, lens);
            }
        }
        if (!weapon) g_typeKey = 0;
    }
    controls::SetRightHandFree(weapon == nullptr);
    static double tPrev = 0; double tNow = NowSec(); float dt = tPrev > 0 ? (float)(tNow - tPrev) : 0.016f; tPrev = tNow;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
    UpdateFalls(dt);
    // left grip + B: weapon away (the game's holster lock, as for climbing) / back; selecting a weapon brings it back too
    {
        bool tog = controls::TakeHolsterToggle() != 0, sel = controls::TakeWeaponSelect() != 0;
        if (controls::VehicleMode() == 0 && playerEnt && g_c.climbHolster) {
            if (tog && !g_userHolster && !g_climbWas) { HolsterLock(playerEnt, true); g_userHolster = true; LOG("hands: weapon put away (left grip + B)"); }
            else if ((tog || sel) && g_userHolster) { g_userHolster = false; if (!g_climbWas) HolsterLock(playerEnt, false); LOG("hands: weapon back (%s)", tog ? "left grip + B" : "weapon selected"); }
            else if (tog && !g_userHolster && g_climbWas) LOG("hands: weapon is already away while climbing");
        }
        // the game's holster lock flag (inventory): also set without the mod - a game saved with the weapon put away
        // comes back with it set, and then neither a weapon nor the map / GPS / phone can come out
        bool gameLocked = false;
        if (g_pawn) {
            uint8_t* inv = *(uint8_t**)(g_pawn + 0x10) + 0x4F0;
            uint8_t* fl = !IsBadReadPtr(inv + 4, 4) ? *(uint8_t**)(inv + 4) : nullptr;
            gameLocked = fl && !IsBadReadPtr(fl + 0x50, 1) && fl[0x50] != 0;
        }
        const bool foreignLock = gameLocked && !g_climbHolstered && !g_climbWas;
        if ((sel || tog) && foreignLock && g_c.climbHolster && playerEnt && controls::VehicleMode() == 0) {   // a weapon chosen: the stale lock goes
            g_climbHolstered = true; HolsterLock(playerEnt, false); g_userHolster = false; LOG("hands: the game had the weapon locked away (not set by the mod) - unlocked for the weapon");
        }
        // the map button with the weapon away: lift the holster lock (without drawing the weapon) so the map / GPS
        // (diamond tracker) or the phone can come out; once the game's normal controls are back, lock it again.
        // A lock the mod didn't set is lifted for good (the game draws the weapon again when the map goes away).
        static bool lifted = false; static double liftT = 0;
        if (controls::TakeMapLift() && !g_climbWas && playerEnt) {
            if (g_userHolster && g_climbHolstered) { HolsterLock(playerEnt, false, false); lifted = true; liftT = tNow; LOG("hands: holster lock lifted for the map / phone"); }
            else if (foreignLock && g_c.climbHolster) { g_climbHolstered = true; HolsterLock(playerEnt, false, false); LOG("hands: the game had the weapon locked away (not set by the mod, e.g. from a saved game) - lock lifted for the map"); }
        }
        // v226: a phone call brings its controls up ~2 s after the button (the map in ~0.2 s): the lock comes back only
        // after the game's map / phone controls have been up and gone again (or none came within 8 s)
        static bool seenMap = false; static double mapGoneT = 0;
        if (lifted) {
            if (controls::MapControls() || controls::PhoneOut()) { seenMap = true; mapGoneT = tNow; }
            bool done = seenMap ? (tNow - mapGoneT > 1.5) : (tNow - liftT > 8.0);
            if (!g_userHolster || g_climbWas) { lifted = false; seenMap = false; }
            else if (done && controls::NormalControls() && !controls::MapOpen() && !controls::PhoneOut() && playerEnt) {
                seenMap = false;
                lifted = false; HolsterLock(playerEnt, true); LOG("hands: map / phone away - weapon kept away");
            }
        }
        controls::SetHandsAway((g_userHolster && !g_climbWas) || (foreignLock && controls::NormalControls()));
    }
    { auto toRef = [&](const float* w, float* out) { Put3(out, f.game.ReferencePosition(Read3(w))); };
      ThrowArcUpdate(f, pd, toWorld, toRef); }
    { float o[3], d[3], p[3], t; if (AimPoint(o, d, p, &t)) { g_aimDist = t; g_aimTick = GetTickCount(); } }   // the aiming dot sits on what the weapon points at
    if (controls::VehicleMode() != 0) {   // in a vehicle: no grabbing, climbing or snap turns (the grips are vehicle controls)
        for (int i = 0; i < 2; ++i) { if (g_held[i].active) { HandleRelease(g_held[i].h); g_held[i] = Held{}; } g_gripPrev[i] = true; }
        controls::TakeSnap(); controls::SetSnapStick(0);
        return;
    }
    for (int hnd = 0; hnd < 2; ++hnd) {
        bool valid = hnd ? f.handValid : f.leftValid;
        float sq = hnd ? f.rSqueeze : f.lSqueeze;
        bool grip = valid && sq > (g_gripPrev[hnd] ? 0.35f : 0.6f);
        bool press = grip && !g_gripPrev[hnd];
        g_gripPrev[hnd] = grip;
        bool free = hnd ? (weapon == nullptr) : !f.twoHand;
        Held& H = g_held[hnd];
        // hand frame in world (rows right, up, back, grip position)
        float hr[3]{}, hu[3]{}, hb[3]{}, hp[3]{};
        if (valid) {
            toWorld(hnd ? f.aimR : f.laimR, hr, false); toWorld(hnd ? f.aimU : f.laimU, hu, false); toWorld(hnd ? f.aimB : f.laimB, hb, false);
            toWorld(hnd ? f.gripO : f.lgripO, hp, true);
        }
        M4 Hm = FromRows(hr, hu, hb, hp);
        if (valid) HistPush(g_hist[hnd], hp, tNow); else g_hist[hnd].n = 0;
        if (g_npc[hnd].on) {   // a hand holding an NPC
            NpcHold& N = g_npc[hnd];
            uint8_t* ent = N.h ? N.h->ent : nullptr;
            bool keep = grip && valid && ent && (NpcDriver(N.comp) > 0 || (N.living && N.body < 0));
            uint8_t** b = nullptr; int c = 0;
            bool have = keep && RagBodies(N.comp, &b, &c);
            if (have && N.body < 0 && NpcDriver(N.comp) > 0) {   // the ragdoll is in the world now: take the body part nearest the hand
                float bd = 1e9f;
                for (int k = 0; k < c; ++k) if (b[k] && !IsBadReadPtr(b[k], 0x1C0)) { float dd = Dist((float*)(b[k] + 0x110), hp); if (dd < bd) { bd = dd; N.body = k; } }
                if (N.body >= 0) { const float* bp = (float*)(b[N.body] + 0x110); for (int k = 0; k < 3; ++k) N.off[k] = bp[k] - hp[k];
                    float ol = sqrtf(Dot3(N.off, N.off)); if (ol > 0.15f) for (float& x : N.off) x *= 0.15f / ol;   // pull it into the hand
                    LOG("npc: %s hand holds body part %d of %d (%.2f m from the hand)", hnd ? "right" : "left", N.body, c, bd); }
            }
            if (keep && have && N.body >= 0 && N.body < c && b[N.body]) {
                uint8_t* rb = b[N.body];
                const float* bp = (float*)(rb + 0x110);
                float gap[3] = { hp[0] + N.off[0] - bp[0], hp[1] + N.off[1] - bp[1], hp[2] + N.off[2] - bp[2] };
                NpcForce* F = N.living ? NpcForceGet(N.comp) : nullptr;
                if (F && F->drv == 3) {   // diagnostics: where the held part, the pelvis and the NPC are
                    static double lastLog = 0;
                    if (tNow - lastLog > 0.15) { lastLog = tNow; float ep[3] = { 0, 0, 0 }; EntPos(ent, ep);
                        const float* p0 = (float*)(b[0] + 0x110);
                        LOG("npc diag: held %d at %.2f %.2f %.2f, hand+off %.2f %.2f %.2f (gap %.2f), body0 %.2f %.2f %.2f, npc %.2f %.2f %.2f, driver %d",
                            N.body, bp[0], bp[1], bp[2], hp[0] + N.off[0], hp[1] + N.off[1], hp[2] + N.off[2], sqrtf(Dot3(gap, gap)), p0[0], p0[1], p0[2], ep[0], ep[1], ep[2], NpcDriver(N.comp)); }
                }
                SoftenGains(hnd, N.comp, F && F->drv == 3 && g_c.npcStiffness < 0.999f);
                NpcDrive drive; Lock(); drive = g_npcDrive[hnd]; Unlock();
                if (F && F->drv == 3) {
                    // held standing: the NPC's feet (its character proxy) follow a sideways pull - he is led by the
                    // arm / collar; a pull down to the ground, or one he can't follow, knocks him down
                    float gh = sqrtf(gap[0] * gap[0] + gap[1] * gap[1]);
                    NpcDrive& D = drive;
                    D.lead[0] = D.lead[1] = D.lead[2] = 0;
                    {   // the hand's own sideways speed (so he keeps pace) plus a correction that closes the gap
                        float hv[3] = { 0, 0, 0 }; HistVel(g_hist[hnd], 0.1, hv);
                        float lx = 0, ly = 0;
                        if (gh > 0.04f) { float sp = (gh - 0.04f) * g_c.npcLeadGain; lx = gap[0] / gh * sp; ly = gap[1] / gh * sp; }
                        if (gh > 0.04f) { lx += hv[0]; ly += hv[1]; }
                        float ls = sqrtf(lx * lx + ly * ly); if (ls > g_c.npcLeadSpeed) { lx *= g_c.npcLeadSpeed / ls; ly *= g_c.npcLeadSpeed / ls; }
                        D.lead[0] = lx; D.lead[1] = ly;
                    }
                    bool down = gap[2] < -g_c.npcKnockDown, tooFar = sqrtf(Dot3(gap, gap)) > g_c.npcKnockPull;
                    if (down || tooFar) { if (N.strain == 0) N.strain = tNow; else if (tNow - N.strain > 0.25) { F->drv = 1; LOG("npc: %s (%.2f m from the hand, %.2f m below)", down ? "pulled to the ground" : "pulled over", sqrtf(Dot3(gap, gap)), -gap[2]); xr::Haptic(hnd, 0.7f, 0.08f); } }
                    else N.strain = 0;
                }
                float v[3]; for (int k = 0; k < 3; ++k) v[k] = gap[k] / dt * g_c.npcGain;
                float sp = sqrtf(Dot3(v, v)); if (sp > g_c.npcMaxSpeed) for (float& x : v) x *= g_c.npcMaxSpeed / sp;
                RbSetVel(rb, v, false);
                {   // the same pull again right after the ragdoll's own frame update (Hook_RagUpd), and the lead for the feet
                    NpcDrive& D = drive;
                    uint8_t* rag = *(uint8_t**)(N.comp + 0x164);
                    D.impl = rag ? *(uint8_t**)(rag + 0x2C) : nullptr; D.rb = rb; memcpy(D.v, v, 12);
                    D.proxy = *(uint8_t**)(N.comp + 0x160); D.tick = GetTickCount();
                    if (!(F && F->drv == 3)) { D.lead[0] = D.lead[1] = D.lead[2] = 0; }
                    D.source = f.source; D.hand = hnd; Lock(); g_npcDrive[hnd] = D; Unlock();
                }
                continue;
            }
            if (keep && !have) continue;   // waiting for the ragdoll
            // let go: the held part keeps the hand's speed (a shove / throw); a living NPC stays down a moment
            if (have && N.body >= 0 && N.body < c && b[N.body]) {
                float tv[3] = { 0, 0, 0 };
                if (HistVel(g_hist[hnd], 0.07, tv)) { for (float& x : tv) x *= g_c.throwScale; float sp = sqrtf(Dot3(tv, tv)); if (sp > g_c.npcMaxSpeed) for (float& x : tv) x *= g_c.npcMaxSpeed / sp; }
                RbSetVel(b[N.body], tv, false);
            } else if (RagBodies(N.comp, &b, &c) && N.body >= 0 && N.body < c && b[N.body]) {
                float tv[3] = { 0, 0, 0 };
                if (HistVel(g_hist[hnd], 0.07, tv)) { for (float& x : tv) x *= g_c.throwScale; float sp = sqrtf(Dot3(tv, tv)); if (sp > g_c.npcMaxSpeed) for (float& x : tv) x *= g_c.npcMaxSpeed / sp; }
                RbSetVel(b[N.body], tv, false);
            }
            SoftenGains(hnd, N.comp, false);
            Lock(); g_npcDrive[hnd] = NpcDrive{}; Unlock();
            const char* why = !grip ? "grip released" : !valid ? "hand lost tracking" : !ent ? "entity gone" : "ragdoll off";
            if (N.living) {
                bool other = g_npc[1 - hnd].on && g_npc[1 - hnd].comp == N.comp;   // still held by the other hand
                NpcForce* F = NpcForceGet(N.comp);
                float hv[3] = { 0, 0, 0 }; HistVel(g_hist[hnd], 0.07, hv);
                bool shove = sqrtf(Dot3(hv, hv)) > g_c.npcKnockSpeed;
                if (F && F->drv == 3 && shove && !other) { F->drv = 1; LOG("npc: shoved over (%.1f m/s)", sqrtf(Dot3(hv, hv))); }
                if (!other) NpcForceSet(N.comp, false, NowSec() + (F && F->drv == 1 ? g_c.npcRagdollTime : 0.3));
            }
            LOG("npc: %s hand let go (%s, %s)", hnd ? "right" : "left", N.living ? "living" : "corpse", why);
            HandleRelease(N.h); N = NpcHold{};
            continue;
        }
        if (g_climb[hnd].on) {   // a hand holding on to a building: nothing else while the grip is held
            if (!grip || !valid) { g_climb[hnd].on = false; LOG("climb: %s hand let go", hnd ? "right" : "left"); }
            else memcpy(g_climb[hnd].now, hp, 12);
            continue;
        }
        if (H.active) {
            uint8_t* ent = H.h ? H.h->ent : nullptr;
            if (!ent || !valid) { HandleRelease(H.h); H = Held{}; continue; }
            bool consumable = H.pickup && *(uintptr_t*)H.pickup == A(0x10E96330);   // CPickupHealth: food, water
            if (consumable && f.headValid && player) {   // bring it to your mouth to eat / drink it
                const float* hpr = hnd ? f.gripO : f.lgripO;
                float hf2[3] = { f.headF[0], 0, f.headF[2] }; Normalize(hf2);
                float mouth[3] = { f.headO[0] + hf2[0] * 0.06f, f.headO[1] - 0.09f, f.headO[2] + hf2[2] * 0.06f };
                if (Dist(hpr, mouth) < g_c.mouthReach) {
                    void** vt = *(void***)H.pickup;
                    typedef bool (__fastcall *Give_t)(void* comp, void* edx, Handle* user, int fromTrigger);
                    typedef void (__fastcall *Despawn_t)(void* comp, void* edx);
                    ++player->ref;
                    bool ok = ((Give_t)vt[0x90 / 4])(H.pickup, nullptr, player, 0);
                    if (ok && *((uint8_t*)H.pickup + 0x17)) ((Despawn_t)vt[0x80 / 4])(H.pickup, nullptr);
                    LOG("hands: consumed at the mouth (%s)", ok ? "ok" : "refused");
                    xr::Haptic(hnd, 0.6f, 0.1f);
                    HandleRelease(H.h); H = Held{};
                    continue;
                }
            }
            if (grip) {   // carry: object keeps the pose it had relative to the hand when grabbed
                M4 m = Mul(H.rel, Hm);
                if (H.dyn) {   // a moving physics body is steered there with its velocity, so it still collides
                    M4 cur; memcpy(cur.m, ent + 0x30, 64);
                    // never into the player's body (heavy objects hurt): keep it outside a column around the head
                    float bodyGap = 1e9f;
                    if (f.headValid) {
                        float hw[3]; toWorld(f.headO, hw, true);
                        float half = fminf(0.5f, H.rg.radius * 0.7f), keep = g_c.bodyRadius + half;
                        float dx = m.m[3][0] - hw[0], dy = m.m[3][1] - hw[1], hd = sqrtf(dx * dx + dy * dy);
                        if (m.m[3][2] < hw[2] + 0.2f && hd < keep) {
                            if (hd < 1e-3f) { dx = f.game.F.x; dy = f.game.F.y; hd = sqrtf(dx * dx + dy * dy); if (hd < 1e-3f) { dx = 1; dy = 0; hd = 1; } }
                            m.m[3][0] = hw[0] + dx / hd * keep; m.m[3][1] = hw[1] + dy / hd * keep;
                        }
                        float cx = cur.m[3][0] - hw[0], cy = cur.m[3][1] - hw[1]; bodyGap = sqrtf(cx * cx + cy * cy) - keep;
                    }
                    float err[3] = { m.m[3][0] - cur.m[3][0], m.m[3][1] - cur.m[3][1], m.m[3][2] - cur.m[3][2] };
                    float v[3]; for (int k = 0; k < 3; ++k) v[k] = err[k] / dt * 0.6f;
                    float vmax = bodyGap < 0.4f ? 2.5f : 12.0f;   // gentle next to the body
                    float sp = sqrtf(Dot3(v, v)); if (sp > vmax) for (float& x : v) x *= vmax / sp;
                    // rotation from the current orientation to the wanted one, as an angular velocity (world)
                    float A3[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { float a = 0; for (int k = 0; k < 3; ++k) a += cur.m[k][i] * m.m[k][j]; A3[i][j] = a; }
                    float ax[3] = { A3[1][2] - A3[2][1], A3[2][0] - A3[0][2], A3[0][1] - A3[1][0] };
                    float c = (A3[0][0] + A3[1][1] + A3[2][2] - 1) * 0.5f; c = c > 1 ? 1 : c < -1 ? -1 : c;
                    float ang = acosf(c), sn = sqrtf(Dot3(ax, ax)), w[3] = { 0, 0, 0 };
                    if (sn > 1e-5f) for (int k = 0; k < 3; ++k) w[k] = ax[k] / sn * ang / dt * 0.5f;
                    float ws = sqrtf(Dot3(w, w)); if (ws > 25.0f) for (float& x : w) x *= 25.0f / ws;
                    RigidSetVel(H.rg, v, w);
                    // stuck far from the hand (wedged behind something) for half a second: let go
                    if (Dot3(err, err) > 0.6f * 0.6f) { if (H.stuck == 0) H.stuck = tNow; else if (tNow - H.stuck > 0.5) { LOG("hands: object stuck, dropped"); HandleRelease(H.h); H = Held{}; } }
                    else H.stuck = 0;
                    continue;
                }
                SetEntTM(ent, m);
                continue;
            }
            // released: over a shoulder = into the backpack, anywhere else = dropped where it is
            float rel[3] = { (hnd ? f.gripO : f.lgripO)[0] - f.headO[0], (hnd ? f.gripO : f.lgripO)[1] - f.headO[1], (hnd ? f.gripO : f.lgripO)[2] - f.headO[2] };
            float hf[3] = { f.headF[0], 0, f.headF[2] }; Normalize(hf);
            float fwd = rel[0] * hf[0] + rel[2] * hf[2];   // ref frame: -z forward, headF already ref
            bool shoulder = f.headValid && fwd < g_c.backpackFwd && rel[1] > g_c.backpackUp;
            if (shoulder && H.pickup && player && *(uintptr_t*)H.pickup != A(0x10E96330)) {
                void** vt = *(void***)H.pickup;
                typedef bool (__fastcall *Give_t)(void* comp, void* edx, Handle* user, int fromTrigger);
                typedef void (__fastcall *Despawn_t)(void* comp, void* edx);
                ++player->ref;
                bool ok = ((Give_t)vt[0x90 / 4])(H.pickup, nullptr, player, 0);
                if (ok && *((uint8_t*)H.pickup + 0x17)) ((Despawn_t)vt[0x80 / 4])(H.pickup, nullptr);
                LOG("hands: %s hand stored the item in the backpack (%s)", hnd ? "right" : "left", ok ? "taken" : "refused");
                xr::Haptic(hnd, ok ? 0.8f : 0.3f, ok ? 0.12f : 0.05f);
                HandleRelease(H.h); H = Held{};
                continue;
            }
            // let go anywhere else: it flies off with the hand's speed (a throw if the hand was moving) and falls
            float tv[3] = { 0, 0, 0 };
            if (HistVel(g_hist[hnd], 0.07, tv)) {
                for (float& x : tv) x *= g_c.throwScale;
                float sp = sqrtf(Dot3(tv, tv));
                if (sp > g_c.throwMaxSpeed) for (float& x : tv) x *= g_c.throwMaxSpeed / sp;
                if (sp < 0.4f) tv[0] = tv[1] = tv[2] = 0;
            }
            float tsp = sqrtf(Dot3(tv, tv));
            if (H.dyn) { float z[3] = { 0, 0, 0 }; RigidSetVel(H.rg, tv, tsp > 0.5f ? nullptr : z); HandleRelease(H.h); }
            else StartFall(H.h, tv);   // the fall owns the handle now
            LOG("hands: %s hand %s the item (%.1f m/s)", hnd ? "right" : "left", tsp > 0.5f ? "threw" : "dropped", tsp);
            if (tsp > 0.5f) xr::Haptic(hnd, 0.3f, 0.04f);
            H = Held{};
            continue;
        }
        if (!press || !valid) continue;   // (a hand holding a weapon can still use things)
        // already climbing: any grip takes a new hold right where the hand is (you never fall while a grip is held)
        if (g_c.climbing && (g_climb[0].on || g_climb[1].on || ClimbHanging())) {
            g_climb[hnd].on = true; memcpy(g_climb[hnd].anchor, hp, 12); memcpy(g_climb[hnd].now, hp, 12); g_climb[hnd].gen = ++g_climbGen;
            xr::Haptic(hnd, 0.5f, 0.05f);
            LOG("climb: %s hand takes the next hold", hnd ? "right" : "left");
            continue;
        }
        // a pickup within reach?
        Handle* hs[64]; int n = free ? OverlapHandles(hp, g_c.grabRadius, hs, 64, g_c.grabMask) : 0;
        if (g_c.grabPhysics && free) {   // loose objects may sit on other collision layers: the game's world-ray layers too
            Handle* h2[32]; int n2 = OverlapHandles(hp, g_c.grabRadius, h2, 32, 0x5BF);
            for (int j = 0; j < n2; ++j) {
                bool dup = false; for (int i = 0; i < n; ++i) if (hs[i]->ent == h2[j]->ent) dup = true;
                if (!dup && n < 64) hs[n++] = h2[j]; else HandleRelease(h2[j]);
            }
        }
        int best = -1; float bestD = 1e9f; void* bestComp = nullptr;
        bool typesReady = *(uint32_t*)A(0x10FDE400) != 0;
        Rigid bestRg; bool bestDyn = false;
        for (int i = 0; i < n; ++i) {
            uint8_t* ent = hs[i]->ent;
            if (!ent || ent == playerEnt || ((*(uint32_t*)(ent + 0x70) >> 4) & 1)) continue;
            void* pc = typesReady ? GetComp(ent, 0x10FDE410) : nullptr;
            Rigid rg; bool hasRg = GetRigid(ent, rg), dyn = hasRg && RigidDynamic(rg);
            bool small = dyn && rg.mass <= g_c.grabMaxMass && rg.radius <= g_c.grabMaxSize;
            if (!pc && !(g_c.grabPhysics && small)) continue;
            if (pc && hasRg && rg.type == 7) continue;   // a pickup fixed to a wall (first-aid box): X uses it, the hands leave it
            if (pc && dyn && !small) dyn = false;
            // how close the hand is: to the object's bounding box for loose objects, to the item itself for pickups
            float ep[3]; EntPos(ent, ep);
            float d = rg.box ? BoxDist(rg, hp) : Dist(ep, hp);
            float reach = rg.box ? g_c.grabReach : g_c.pickupReach;
            if (d > reach) continue;
            if (d < bestD) { bestD = d; best = i; bestComp = pc; bestRg = rg; bestDyn = dyn; }
        }
        if (best < 0 && n > 0) {   // tell what was there (to tune what counts as grabbable)
            static int logged = 0;
            if (logged < 40) for (int i = 0; i < n && i < 6; ++i) {
                uint8_t* ent = hs[i]->ent; if (!ent || ent == playerEnt) continue; ++logged;
                Rigid rg; bool has = GetRigid(ent, rg);
                void* comp = *(uint32_t*)A(0x10FDE930) ? GetComp(ent, 0x10FDE940) : nullptr;
                float ep[3]; EntPos(ent, ep);
                LOG("hands: nothing grabbed: entity %p phys comp %p vtbl %08x bodies %d type %d mass %.1f size %.2f, hand %.2f m from its box, %.2f m from its origin", (void*)ent, comp,
                    comp ? (unsigned)(*(uintptr_t*)comp - g_base + 0x10000000) : 0u, has ? rg.n : 0, rg.type, rg.n ? rg.mass : 0.0f, rg.radius, BoxDist(rg, hp), Dist(ep, hp));
            }
        }
        for (int i = 0; i < n; ++i) if (i != best) HandleRelease(hs[i]);
        if (best >= 0) {
            H.h = hs[best]; H.pickup = bestComp; H.active = true; H.rg = bestRg; H.dyn = bestDyn;
            for (auto& fl : g_fall) if (fl.active && fl.h && fl.h->ent == H.h->ent) { HandleRelease(fl.h); fl = Fall{}; }   // caught in the air
            M4 em; memcpy(em.m, H.h->ent + 0x30, 64); em.m[0][3] = em.m[1][3] = em.m[2][3] = 0; em.m[3][3] = 1;
            M4 hinv; if (!Inv(Hm, hinv)) { HandleRelease(H.h); H = Held{}; continue; }
            H.rel = Mul(em, hinv);
            if (!H.dyn) { H.rel.m[3][0] = 0; H.rel.m[3][1] = -0.03f; H.rel.m[3][2] = -0.04f; }   // pickups sit in the palm
            xr::Haptic(hnd, 0.5f, 0.05f);
            LOG("hands: %s hand picked up %s %.2f m away (body type %d, %.1f kg, radius %.2f m)", hnd ? "right" : "left", bestComp ? "a pickup" : "an object", bestD, H.rg.type, H.rg.n ? H.rg.mass : 0.0f, H.rg.radius);
            continue;
        }
        // an NPC (or a body) at the hand: take hold of the nearest body part
        if (g_c.npcGrab && free && !(hnd && controls::RightHandInHolsterZone())) {
            uint8_t* nc = nullptr; float nd = 0;
            Handle* nh = FindNpc(hp, playerEnt, &nc, &nd);
            if (nh) {
                NpcHold& N = g_npc[hnd]; N = NpcHold{}; N.on = true; N.h = nh; N.comp = nc;
                int drv = NpcDriver(nc);
                NpcForce* F0 = NpcForceGet(nc);
                N.living = F0 != nullptr || drv != 1;   // knocked down by us = still alive
                if (N.living) NpcForceSet(nc, true, 0, nh->ent, F0 ? F0->drv : (g_c.npcActive ? 3 : 1));   // the game's next ragdoll update (every frame) takes it there
                xr::Haptic(hnd, 0.6f, 0.06f);
                LOG("npc: %s hand grabs %s %.2f m away (entity %p, driver %d -> %d)", hnd ? "right" : "left", N.living ? "a living NPC" : "a body", nd, (void*)nh->ent, drv, NpcDriver(nc));
                continue;
            }
        }
        // a building at the hand: hold on to it (climbing)
        if (g_c.climbing && !(hnd && controls::RightHandInHolsterZone()) && ClimbProbe(hp, hr, hb, hnd, playerEnt)) {
            g_climb[hnd].on = true; memcpy(g_climb[hnd].anchor, hp, 12); memcpy(g_climb[hnd].now, hp, 12); g_climb[hnd].gen = ++g_climbGen;
            xr::Haptic(hnd, 0.5f, 0.05f);
            LOG("climb: %s hand holds on", hnd ? "right" : "left");
            continue;
        }
        // nothing grabbed: the grip is the game's interact (E / pad Y) - doors, turrets, weapons on the ground. The game
        // decides whether something is in reach. Not for the left hand holding the gun, not for holster grabs.
        if (!g_c.gripInteract) continue;   // interact is on X (GripInteract=0)
        if (!hnd && !free) continue;
        if (hnd && controls::RightHandInHolsterZone()) continue;
        g_useRequest = true; if (g_useCallback) g_useCallback();
        LOG("hands: %s grip -> interact", hnd ? "right" : "left");
    }
    if (g_c.contactPush) ContactPush(f, playerEnt, toWorld, weapon);
    MacheteSwing(f, playerEnt, toWorld, weapon);
    if (g_c.climbing) {
        ClimbUpdate(playerEnt, f, dt);
    }
    SnapUpdate(playerEnt, dt);
    RoomScaleUpdate(playerEnt, dt);
}

typedef void (__fastcall *Hier_t)(void* skel);
static Hier_t o_Hier = nullptr;
static void __fastcall Hook_Hier(void* skel) { o_Hier(skel); Apply((uint8_t*)skel); }

typedef void (__cdecl *HierJob_t)(void* job);
static HierJob_t o_HierJob = nullptr;
static void __cdecl Hook_HierJob(void* job) {
    o_HierJob(job);
    uint8_t* s = (uint8_t*)g_playerSkel;
    if (!s) return;
    // the job works on a bone array; match it with the player's skeleton
    typedef void* (__fastcall *JobGet_t)(void* job, void* edx, int i);
    void* arr = ((JobGet_t)A(0x102AF580))(job, nullptr, 0);
    if (arr && arr == *(void**)(s + 0x20)) Apply(s);
}

// ---------------------------------------------------------------- shots
typedef void (__fastcall *PlayerShot_t)(void* self, void* edx, void* pawn, float* origin, float* dir, int p4);
static PlayerShot_t o_PlayerShot = nullptr;
static void __fastcall Hook_PlayerShot(void* self, void* edx, void* pawn, float* origin, float* dir, int p4) {
    o_PlayerShot(self, edx, pawn, origin, dir, p4);
    if (!g_c.aimFromController || !origin || !dir) return;
    if (controls::MountedWeapon()) {   // v221: a mounted gun fires along the game's camera line (where its dot is), not the hand's
        static int nl = 0; if (nl++ < 3) LOG("arms: mounted gun shot left as the game aims it (dir %.3f %.3f %.3f)", dir[0], dir[1], dir[2]); return; }
    float ao[3], af[3], cfw[3];
    if (!ReadAim(ao,af,cfw)) return;
    // keep the game's spread: the deviation of its direction from the camera's forward
    float nd[3]; for (int k = 0; k < 3; ++k) nd[k] = af[k] + (dir[k] - cfw[k]);
    Normalize(nd);
    for (int k = 0; k < 3; ++k) { origin[k] = ao[k]; dir[k] = nd[k]; }
    if (InterlockedIncrement(&g_shots) <= 3) LOG("arms: shot from controller %.2f %.2f %.2f dir %.3f %.3f %.3f", ao[0], ao[1], ao[2], nd[0], nd[1], nd[2]);
}

// Projectile weapons (crossbow, rocket and grenade launchers): CWeaponFireProjectileStrategy, player branch
// 0x101998F0 (thiscall: shooter pawn, out muzzle position, out angles, out "hit" flag, out hit point, bool; ret 0x18).
// The game aims from the muzzle at what its camera's ray (250 m) hits; here the projectile still leaves the muzzle,
// aimed at what the weapon's sight line (or the scope) points at. (0x10199CA0 is the AI branch.)
// Angles from a direction: 0x100506F0 (thiscall angles, dir).
typedef void (__fastcall *ProjAim_t)(void* self, void* edx, void* pawn, float* origin, float* angles, uint8_t* hitFlag, float* hitPos, int p6);
static ProjAim_t o_ProjAim = nullptr;
static void __fastcall Hook_ProjAim(void* self, void* edx, void* pawn, float* origin, float* angles, uint8_t* hitFlag, float* hitPos, int p6) {
    o_ProjAim(self, edx, pawn, origin, angles, hitFlag, hitPos, p6);
    static int calls = 0; bool logIt = calls++ < 5;
    if (!g_c.aimFromController || !origin || !angles || controls::MountedWeapon()) return;   // v221: mounted guns as the game aims them
    float o[3], d[3], p[3], t = 0;
    if (!AimPoint(o, d, p, &t)) { if (logIt) LOG("arms: projectile - no sight line"); return; }
    float nd[3] = { p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
    if (Dot3(nd, nd) < 1e-6f) return;
    Normalize(nd);
    if (Dot3(nd, d) < 0.9f) memcpy(nd, d, 12);   // the muzzle far off the sight line (target right at the muzzle): the sight direction
    typedef void (__fastcall *Ang_t)(float* angles, void* edx, const float* dir);
    ((Ang_t)A(0x100506F0))(angles, nullptr, nd);
    if (hitFlag && hitPos && t < 299.0f) { *hitFlag = 1; memcpy(hitPos, p, 12); }
    else if (hitFlag) *hitFlag = 0;
    g_diagUntil = GetTickCount64() + 3500;
    if (logIt) LOG("arms: projectile from muzzle %.2f %.2f %.2f dir %.3f %.3f %.3f -> sight point %.1f m away", origin[0], origin[1], origin[2], nd[0], nd[1], nd[2], Dist(origin, p));
}

// ---------------------------------------------------------------- who moves the crossbow (diagnostic)
// Cheat Engine found the writers: the entity world setters 0x104DF850 / 0x104DF440 (thiscall entity, matrix, flags;
// ret 8) for the crossbow object, and the quaternion+position -> matrix routine 0x100D7B10 (thiscall quat, out, pos;
// ret 8) for its model's bones. All are shared by everything, so their callers are counted for the held weapon only,
// in and out of the 3.5 s after a projectile shot, and logged.
struct CallerHist { uintptr_t ra[24]; int cnt[24]; int n; };
static CallerHist g_chEnt[2], g_chBone[2];
static void HistAdd(CallerHist& h, uintptr_t ra) {
    int nn = h.n; if (nn > 24) nn = 24; for (int i = 0; i < nn; ++i) if (h.ra[i] == ra) { ++h.cnt[i]; return; }
    int k = h.n; if (k >= 0 && k < 24) { h.ra[k] = ra; h.cnt[k] = 1; h.n = k + 1; }
}
static void HistLog(const char* what, CallerHist& h) {
    char b[700]; int o = snprintf(b, sizeof b, "arms: callers %s:", what);
    for (int i = 0; i < h.n && o < 680; ++i) o += snprintf(b + o, sizeof b - o, " %08x x%d", (unsigned)(h.ra[i] - g_base + 0x10000000), h.cnt[i]);
    LOG("%s", b); h.n = 0;
}
static uint8_t* HeldEnt() {
    uint8_t* g = g_wGfx; if (!g || IsBadReadPtr(g + 8, 4)) return nullptr;
    uint8_t* h = *(uint8_t**)(g + 8); if (!h || IsBadReadPtr(h + 12, 4)) return nullptr;
    return *(uint8_t**)(h + 12);
}
typedef void (__fastcall *EntSet_t)(void* ent, void* edx, const float* m, int fl);
static EntSet_t o_EntSetA = nullptr, o_EntSetB = nullptr;
static void __fastcall Hook_EntSetA(void* ent, void* edx, const float* m, int fl) {
    if (ent && ent == HeldEnt()) HistAdd(g_chEnt[GetTickCount64() < g_diagUntil], (uintptr_t)__builtin_return_address(0));
    o_EntSetA(ent, edx, m, fl);
}
static void __fastcall Hook_EntSetB(void* ent, void* edx, const float* m, int fl) {
    if (ent && ent == HeldEnt()) HistAdd(g_chEnt[GetTickCount64() < g_diagUntil], (uintptr_t)__builtin_return_address(0) + 1);   // +1: told apart from setter A
    o_EntSetB(ent, edx, m, fl);
}
typedef void (__fastcall *QuatMat_t)(const float* q, void* edx, float* out, const float* pos);
static QuatMat_t o_QuatMat = nullptr;
static void __fastcall Hook_QuatMat(const float* q, void* edx, float* out, const float* pos) {
    uint8_t* sk = g_wSkel;
    if (sk && out) {
        Bone* b = *(Bone**)(sk + 0x20); int n = *(int*)(sk + 0x2C);
        if (b && n > 0 && n < 64 && (uint8_t*)out >= (uint8_t*)b && (uint8_t*)out < (uint8_t*)(b + n))
            HistAdd(g_chBone[GetTickCount64() < g_diagUntil], (uintptr_t)__builtin_return_address(0));
    }
    o_QuatMat(q, edx, out, pos);
}
// The held weapon hangs from its parent through an attachment node ([entity+0x78]; the attachment update 0x10553500
// computes child world = offset(+0x80) * parent bone (index +0x1C, the parent's skeleton) * parent world, 0x105530F0).
// Logged whenever the bone or the offset changes; with FreezeReload the normal bone/offset are put back while a
// projectile reload runs.
static volatile bool g_attachForced = false;          // the held weapon was moved back onto the hand this frame
static float g_wRestBones[64][16]; static volatile int g_wRestN = 0; static Bone* volatile g_wRestBase = nullptr;
static void AttachWatch() {
    uint8_t* ent = HeldEnt();
    static uint8_t* lastEnt = nullptr; static float handOff[16]; static bool haveOff = false; static int lastIdx = -3; static ULONGLONG onHandSince = 0;
    int hand = g_slot[12];
    if (!ent || IsBadReadPtr(ent + 0x78, 4) || hand < 0) { lastEnt = nullptr; g_attachForced = false; g_wRestN = 0; return; }
    uint8_t* at = *(uint8_t**)(ent + 0x78);
    if (!at || IsBadReadPtr(at, 0xC0)) return;
    int idx = *(int*)(at + 0x1C); float* off = (float*)(at + 0x80);
    ULONGLONG t = GetTickCount64();
    // the hand pose of each weapon is kept (a weapon drawn again reloads straight away - the crossbow does - before a
    // new one could be taken)
    struct Saved { uint8_t* ent; float off[16]; float bones[64][16]; int n; Bone* base; };
    static Saved saved[8]; static int nSaved = 0;
    if (ent != lastEnt) {
        if (lastEnt && haveOff && g_wRestN > 0) {   // keep the previous weapon's
            int k = 0; while (k < nSaved && saved[k].ent != lastEnt) ++k;
            if (k == nSaved) { if (nSaved < 8) ++nSaved; else k = 0; }
            saved[k].ent = lastEnt; memcpy(saved[k].off, handOff, 64); memcpy(saved[k].bones, g_wRestBones, sizeof g_wRestBones); saved[k].n = g_wRestN; saved[k].base = g_wRestBase;
        }
        lastEnt = ent; haveOff = false; lastIdx = -3; onHandSince = t; g_wRestN = 0;
        for (int k = 0; k < nSaved; ++k) if (saved[k].ent == ent) {
            memcpy(handOff, saved[k].off, 64); memcpy(g_wRestBones, saved[k].bones, sizeof g_wRestBones); g_wRestBase = saved[k].base; g_wRestN = saved[k].n; haveOff = true;
            LOG("arms: held weapon %p - its hand pose from before is used", ent); break;
        }
    }
    // the model's bone array must still be the saved one
    { uint8_t* sk = g_wSkel; if (g_wRestN > 0 && (!sk || IsBadReadPtr(sk + 0x20, 4) || *(Bone**)(sk + 0x20) != g_wRestBase)) { g_wRestN = 0; haveOff = false; } }
    if (idx != lastIdx) { static int nl = 0; if (nl++ < 40) LOG("arms: held weapon attachment %p: game puts it on bone %d (hand %d)", at, idx, hand); lastIdx = idx; if (idx == hand) onHandSince = t; }
    // the weapon's place on the hand and its model's pose there, taken once it has hung on the hand for a moment
    static ULONGLONG forcedUntil = 0;
    if (idx == hand && t < forcedUntil) { if (g_diagUntil + 300 > forcedUntil) forcedUntil = g_diagUntil + 300; return; }   // still our pin: keep the model's pose held (through the reload after the shot)
    if (idx == hand) {
        g_attachForced = false;
        // v207: each weapon type's place in the hand is recorded once (normal controls, settled, saved in [Holds] O<key>)
        // and restored whenever the game re-attaches the weapon turned or shifted (after a phone call, the map, climbing,
        // putting it away, a cut scene) - the weapon sat tilted in the steady hand until left grip + A.
        if (g_c.seatWeapon && g_typeKey) {
            uint8_t* skx = g_wSkel; int nb = (skx && !IsBadReadPtr(skx + 0x2C, 4)) ? *(int*)(skx + 0x2C) : 0;
            (void)nb; uint32_t rk = g_typeKey;
            static uint32_t refKey = 0; static float ref[16]; static bool haveRef = false, triedLoad = false; static uint8_t* refEnt = nullptr;
            static ULONGLONG normalSince = 0; static int fixes = 0;
            if (rk != refKey || ent != refEnt) { refKey = rk; refEnt = ent; haveRef = false; triedLoad = false; }
            bool normal = controls::NormalControls() && !ReloadActive() && t > g_diagUntil + 300;
            if (!normal) normalSince = t;
            if (!haveRef && !triedLoad && g_ini[0]) {
                triedLoad = true; char k[16], v[512]; snprintf(k, sizeof k, "O%08X", rk);
                GetPrivateProfileStringA("Holds", k, "", v, sizeof v, g_ini);
                if (v[0] && sscanf(v, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &ref[0], &ref[1], &ref[2], &ref[3], &ref[4], &ref[5], &ref[6], &ref[7],
                                   &ref[8], &ref[9], &ref[10], &ref[11], &ref[12], &ref[13], &ref[14], &ref[15]) == 16) { haveRef = true; LOG("arms: weapon %p uses its saved place in the hand (%08x)", ent, rk); }
            }
            float ang = 0, dist = 0;
            if (haveRef) {
                float tr = 0; for (int r = 0; r < 3; ++r) { float a[3] = { off[r * 4], off[r * 4 + 1], off[r * 4 + 2] }, b[3] = { ref[r * 4], ref[r * 4 + 1], ref[r * 4 + 2] };
                    float la = sqrtf(Dot3(a, a)), lb = sqrtf(Dot3(b, b)); if (la > 1e-6f && lb > 1e-6f) tr += Dot3(a, b) / (la * lb); }
                ang = acosf(fmaxf(-1.0f, fminf(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
                float d[3] = { off[12] - ref[12], off[13] - ref[13], off[14] - ref[14] }; dist = sqrtf(Dot3(d, d));
            }
            if (!haveRef && normal && t - onHandSince > 1500 && t - normalSince > 2000) {   // the first normal hold: record it
                memcpy(ref, off, 64); haveRef = true;
                char k[16], v[512]; int o = 0; snprintf(k, sizeof k, "O%08X", rk);
                for (int i = 0; i < 16 && o < (int)sizeof v - 20; ++i) o += snprintf(v + o, sizeof v - o, "%s%.6f", i ? " " : "", ref[i]);
                if (g_ini[0]) WritePrivateProfileStringA("Holds", k, v, g_ini);
                LOG("arms: weapon %p place in the hand recorded (%08x)", ent, rk);
            } else if (haveRef && normal && t - onHandSince > 400 && (ang > 1.0f || dist > 0.005f) && ang < 150.0f && dist < 0.5f) {
                memcpy(off, ref, 64);
                if (fixes++ < 60) LOG("arms: weapon %p re-seated in the hand (was %.1f deg / %.1f cm off)", ent, ang, dist * 100.0f);
            }
        }
        if (t - onHandSince > 1500 && t > g_diagUntil) {
            memcpy(handOff, off, 64); haveOff = true;
            uint8_t* sk = g_wSkel;
            if (sk && !IsBadReadPtr(sk + 0x2C, 4)) { int n = *(int*)(sk + 0x2C); Bone* bb = *(Bone**)(sk + 0x20);
                if (bb && n > 0 && n <= 64 && !IsBadReadPtr(bb, n * sizeof(Bone))) { for (int i = 0; i < n; ++i) memcpy(g_wRestBones[i], bb[i].model, 64); g_wRestBase = bb; g_wRestN = n; } }
        }
        return;
    }
    // the game hung it elsewhere (the crossbow's bolt reload: the spine, or the camera while scoped): back on the hand,
    // with the model held in its pose there (Hook_PoseBones) - in VR the hand holds the weapon throughout
    g_attachForced = false;
    // (only with no offset of its own: putting the weapon away hangs it on the spine WITH an offset - the back holster)
    bool noOff = fabsf(off[12]) < 0.001f && fabsf(off[13]) < 0.001f && fabsf(off[14]) < 0.001f;
    if (haveOff && g_wRestN > 0 && g_c.freezeReload && controls::NormalControls() && noOff) {
        *(int*)(at + 0x1C) = hand; memcpy(off, handOff, 64);
        g_attachForced = true;
        { ULONGLONG a = t + 600, b2 = g_diagUntil + 300; forcedUntil = a > b2 ? a : b2; }
        return;
        static int nf = 0; if (nf++ < 10) LOG("arms: held weapon put back on the hand (the game moved it to bone %d)", idx);
    }
}
// The weapon models' animated bones are set straight from the animation pose by 0x1061FB70 (cdecl count, bone index
// map, poses, bone array). While the held weapon is kept on the hand, its model keeps the pose it had there.
typedef void (__cdecl *PoseBones_t)(int count, const int* map, const float* poses, Bone* bones);
static PoseBones_t o_PoseBones = nullptr;
static void __cdecl Hook_PoseBones(int count, const int* map, const float* poses, Bone* bones) {
    o_PoseBones(count, map, poses, bones);
    if (g_attachForced && bones && bones == g_wRestBase) { int n = g_wRestN; for (int i = 0; i < n; ++i) memcpy(bones[i].model, g_wRestBones[i], 64); }
}
static void CallerReport() {   // game thread, each frame
    static bool was = false; bool w = GetTickCount64() < g_diagUntil;
    if (was && !w) { HistLog("of the held weapon's world setter AFTER a shot", g_chEnt[1]); HistLog("of its bone matrices AFTER a shot", g_chBone[1]);
                     HistLog("of the world setter normally", g_chEnt[0]); HistLog("of its bone matrices normally", g_chBone[0]); }
    was = w;
}


// v223 "brown filter" and rim lighting. The environment's per-frame adaptive bloom / post settings (time of day blend,
// transitions, scripted overrides) are produced by 0x101177D0 (thiscall(env, float), ret 4) into the render setting
// it gets with 0x10455410 thiscall(mgr 0x10F96AD8, env + 0x3A0, 1): +0x2C/+0x30/+0x34 fColorRemapRed/Green/Blue,
// +0x38 fContrast, +0x3C fSaturation. Neutral (the ModDB tutorial's world .fcb values): remap 0, contrast 0,
// saturation 1. The lighting (sun, ambient, rim) is blended by 0x10119100 thiscall(env, float, out*) (ret 8):
// out +0x60 = fRimLightIntensity.
typedef void (__fastcall *EnvBloom_t)(void* env, void* edx, float w);
static EnvBloom_t o_EnvBloom = nullptr;
static void __fastcall Hook_EnvBloom(void* env, void* edx, float w) {
    o_EnvBloom(env, edx, w);
    if (!g_c.noColorFilter || !env) return;
    typedef uint8_t* (__fastcall *Get_t)(void* mgr, void* edx, void* h, int wr);
    uint8_t* o = ((Get_t)A(0x10455410))((void*)A(0x10F96AD8), nullptr, (uint8_t*)env + 0x3A0, 1);
    if (!o || IsBadWritePtr(o + 0x2C, 0x14)) return;
    float* f = (float*)(o + 0x2C);
    static int nl = 0; if (nl++ < 2) LOG("colour filter off: remap %.2f %.2f %.2f contrast %.2f saturation %.2f -> neutral", f[0], f[1], f[2], f[3], f[4]);
    f[0] = f[1] = f[2] = 0.0f; f[3] = 0.0f; f[4] = 1.0f;
}
typedef void (__fastcall *EnvLight_t)(void* env, void* edx, float t, uint8_t* out);
static EnvLight_t o_EnvLight = nullptr;
static void __fastcall Hook_EnvLight(void* env, void* edx, float t, uint8_t* out) {
    o_EnvLight(env, edx, t, out);
    if (!g_c.noRimLight || !out || IsBadWritePtr(out + 0x60, 4)) return;
    static int nl = 0; if (nl++ < 2) LOG("rim light off (was %.2f)", *(float*)(out + 0x60));
    *(float*)(out + 0x60) = 0.0f;
}
// ---------------------------------------------------------------- install
void Install(uintptr_t base, const Config& c, PatchFn patch) {
    g_base = base; g_c = c;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    for (int i = 0; i < 21; ++i) g_slot[i] = -1;
    if (!c.weaponInHand && !c.aimFromController && !c.dump) { LOG("arms: off"); return; }
    static const uint8_t pro[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };   // push ebp; mov ebp,esp; and esp,-16
    bool a = patch(A(0x10693C00), pro, 6, (void*)&Hook_CamUpdate, (void**)&o_CamUpdate);
    bool b = patch(A(0x103322C0), pro, 6, (void*)&Hook_Hier, (void**)&o_Hier);
    bool d = patch(A(0x1064EC50), pro, 6, (void*)&Hook_HierJob, (void**)&o_HierJob);
    bool e = c.aimFromController ? patch(A(0x10112600), pro, 6, (void*)&Hook_PlayerShot, (void**)&o_PlayerShot) : false;
    bool pj = c.aimFromController ? patch(A(0x101998F0), pro, 6, (void*)&Hook_ProjAim, (void**)&o_ProjAim) : false;
    LOG("arms: projectile aim hook %d", pj);
    LOG("arms: weapon pose hook %d", patch(A(0x1061FB70), pro, 6, (void*)&Hook_PoseBones, (void**)&o_PoseBones));
    LOG("arms: monocular hook %d", patch(A(0x106EFB40), pro, 6, (void*)&Hook_EquipMono, (void**)&o_EquipMono));
    if (c.noColorFilter) { static const uint8_t proB[5] = { 0x83, 0xEC, 0x08, 0x56, 0x57 };   // sub esp,8; push esi; push edi
        LOG("arms: colour filter off hook %d", patch(A(0x101177D0), proB, 5, (void*)&Hook_EnvBloom, (void**)&o_EnvBloom)); }
    if (c.noRimLight) LOG("arms: rim light off hook %d", patch(A(0x10119100), pro, 6, (void*)&Hook_EnvLight, (void**)&o_EnvLight));
    {   static const uint8_t proP[6] = { 0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1 };   // sub esp,18h; push esi; mov esi,ecx
        bool m1 = patch(A(0x1072C260), pro, 6, (void*)&Hook_MonoUpd, (void**)&o_MonoUpd);
        bool m2 = patch(A(0x1010D780), proP, 6, (void*)&Hook_AimPos, (void**)&o_AimPos);
        bool m3 = patch(A(0x1010D850), pro, 6, (void*)&Hook_AimRot, (void**)&o_AimRot);
        LOG("arms: monocular tag ray hooks %d %d %d", m1, m2, m3); }
    { bool h1 = patch(A(0x104DF850), pro, 6, (void*)&Hook_EntSetA, (void**)&o_EntSetA), h2 = patch(A(0x104DF440), pro, 6, (void*)&Hook_EntSetB, (void**)&o_EntSetB), h3 = patch(A(0x100D7B10), pro, 6, (void*)&Hook_QuatMat, (void**)&o_QuatMat); LOG("arms: crossbow caller diagnostics %d %d %d", h1, h2, h3); }
    if (c.freezeReload) LOG("arms: reload detour %d", InstallReloadDetour());
    if (c.freezeReload) { static const uint8_t proA[8] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0xFF, 0x56 }; LOG("arms: animation event hook %d", patch(A(0x101FA340), proA, 8, (void*)&Hook_AnimEvt, (void**)&o_AnimEvt)); }
    static const uint8_t proW[7] = { 0xD9, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x08 };   // fld [esp+4]; sub esp,8
    bool w = patch(A(0x1072DFD0), proW, 7, (void*)&Hook_WatchUpd, (void**)&o_WatchUpd);
    static const uint8_t proR[10] = { 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x64, 0x01, 0x00, 0x00, 0x00 };   // push esi; mov esi,ecx; cmp [esi+0x164],0
    bool sr = patch(A(0x10544380), proR, 10, (void*)&Hook_CharSetRot, (void**)&o_CharSetRot);
    bool th = patch(A(0x101AA3A0), pro, 6, (void*)&Hook_ThrowCalc, (void**)&o_ThrowCalc);
    static const uint8_t proD[7] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x0C };   // mov eax,[esp+4]; sub esp,0Ch
    bool sd = c.npcGrab ? patch(A(0x10547600), proD, 7, (void*)&Hook_SetDriver, (void**)&o_SetDriver) : false;
    bool ru = c.npcGrab ? patch(A(0x104B4D60), pro, 6, (void*)&Hook_RagUpd, (void**)&o_RagUpd) : false;
    static const uint8_t proP[5] = { 0x8B, 0x49, 0x2C, 0x8B, 0x01 };   // mov ecx,[ecx+2Ch]; mov eax,[ecx]
    bool pm = c.npcGrab ? patch(A(0x10493920), proP, 5, (void*)&Hook_ProxyMove, (void**)&o_ProxyMove) : false;
    LOG("arms: NPC grab %d (ragdoll driver hook %d, ragdoll update hook %d, movement hook %d)", c.npcGrab, sd, ru, pm);
    LOG("arms: throw hook %d", th);
    LOG("arms: orientation hook %d", sr);
    LOG("arms: watch hook %d", w);
    LOG("arms: hooks camera %d hierarchy %d hierarchy-job %d shots %d (weapon in hand %d, aim from controller %d)", a, b, d, e, c.weaponInHand, c.aimFromController);
}
bool CameraRay(float o[3], float d[3]) { if (!g_csInit) return false; fc2vr::authority::Stamp source; void* weapon;
    Lock(); bool ok = g_shotValid && g_rayTick && GetTickCount()-g_rayTick < 200; source = g_shotSource; weapon = g_shotWeapon;
    memcpy(o,g_camRefO,12); memcpy(d,g_camRefD,12); Unlock(); return ok && ShotCurrent(source,weapon); }
bool ProjectileReloadActive() { return GetTickCount64() < g_diagUntil; }
float AimDistance() { return g_aimTick && GetTickCount() - g_aimTick < 300 ? g_aimDist : 0.0f; }
bool SightRay(float o[3], float d[3], RayContext* context) { if (!g_csInit) return false; fc2vr::authority::Stamp source; void* weapon;
    Lock(); bool ok = g_shotValid && g_rayTick && GetTickCount()-g_rayTick < 200; source = g_shotSource; weapon = g_shotWeapon;
    memcpy(o,g_rayRefO,12); memcpy(d,g_rayRefD,12); if (context) *context = {source,weapon}; Unlock(); return ok && ShotCurrent(source,weapon); }
static bool MonoAimRay(float o[3], float d[3]) {
    if (!g_inMonoUpd || !controls::MonocularAtFace() || !g_csInit) return false;
    Lock(); bool ok = g_scopeTick && GetTickCount() - g_scopeTick < 250; memcpy(o, g_scopeO, 12); memcpy(d, g_scopeD, 12); Unlock();
    return ok;
}
// v216: the drawn monocular moved with the scope camera, right before the scope view is drawn (same frame: no lag).
void MonoRenderFix(const float game[16], const float posed[16]) {
    uint8_t* gfx = g_monoGfx;
    if (!gfx || !controls::MonocularAtFace() || IsBadWritePtr(gfx + 0x170, 64)) return;
    static float lastW[16], lastBase[16]; static bool wrote = false;
    float cur[16]; memcpy(cur, gfx + 0x170, 64);
    if (wrote && !memcmp(cur, lastW, 64)) memcpy(cur, lastBase, 64);   // the game didn't place it again since our write
    else memcpy(lastBase, cur, 64);
    M4 Wm, G, P, iG; memcpy(Wm.m, cur, 64); memcpy(G.m, game, 64); memcpy(P.m, posed, 64);
    float w3[4] = { Wm.m[0][3], Wm.m[1][3], Wm.m[2][3], Wm.m[3][3] };
    M4* ms[3] = { &Wm, &G, &P }; for (M4* m : ms) { m->m[0][3] = m->m[1][3] = m->m[2][3] = 0; m->m[3][3] = 1; }
    if (!Inv(G, iG)) return;
    M4 o = Mul(Mul(Wm, iG), P);
    o.m[0][3] = w3[0]; o.m[1][3] = w3[1]; o.m[2][3] = w3[2]; o.m[3][3] = w3[3];
    memcpy(gfx + 0x170, o.m, 64); memcpy(lastW, o.m, 64); wrote = true;
}
bool MonoEquipped() { return g_monoAt != nullptr; }
bool MonoTarget() { return g_monoTarget && GetTickCount() - g_monoTargetTick < 300; }
void SetScopeRay(const float o[3], const float d[3], const RayContext& context) { if (!g_csInit) return;
    Lock(); memcpy(g_scopeO,o,12); memcpy(g_scopeD,d,12); g_scopeSource = context.source; g_scopeWeapon = context.weapon; g_scopeTick = GetTickCount(); Unlock(); }
void* CurrentWeapon() { return g_curWeapon; }
void SetMachete(float angle, float yaw, float roll) { g_c.macheteAngle = angle; g_c.macheteYaw = yaw; g_c.macheteRoll = roll; }
void GetMachete(float& angle, float& yaw, float& roll) { angle = g_c.macheteAngle; yaw = g_c.macheteYaw; roll = g_c.macheteRoll; }
void SetMapHands(float angle, float yaw, float roll) { g_c.mapAngle = angle; g_c.mapYaw = yaw; g_c.mapRoll = roll; }
void GetMapHands(float& angle, float& yaw, float& roll) { angle = g_c.mapAngle; yaw = g_c.mapYaw; roll = g_c.mapRoll; }
void SetFreeHands(float angle, float yaw, float roll) { g_c.freePitch = angle; g_c.freeYaw = yaw; g_c.freeRoll = roll; }
void GetFreeHands(float& angle, float& yaw, float& roll) { angle = g_c.freePitch; yaw = g_c.freeYaw; roll = g_c.freeRoll; }
void SetPhoneHands(float angle, float yaw, float roll) { g_c.phonePitch = angle; g_c.phoneYaw = yaw; g_c.phoneRoll = roll; }
void GetPhoneHands(float& angle, float& yaw, float& roll) { angle = g_c.phonePitch; yaw = g_c.phoneYaw; roll = g_c.phoneRoll; }
void SetUseCallback(void (*cb)()) { g_useCallback = cb; }
void SetWeaponKey(uint32_t k) { g_weaponKey = k; }
void SetIniPath(const char* p) {
    snprintf(g_ini, sizeof g_ini, "%s", p);
}
float LastHandSpan() { return g_lastSpan; }
bool Foregrip(float out[3], bool& oneHanded) { if (!g_csInit) return false; Lock(); bool v = g_foregripValid; memcpy(out, g_foregripRef, 12); oneHanded = g_oneHanded; Unlock(); return v; }
// v209 steady camera: the player entity's position (its world matrix translation, +0x60) and the game's own
// effective heading (pawn data +0x310, radians) - read without calling into the game (any thread)
bool PlayerState(float pos[3], float* heading) {
    uint8_t* e = g_playerEntR; uint8_t* p = g_pawn;
    if (!e || !p || IsBadReadPtr(e + 0x60, 12) || IsBadReadPtr(p + 0x10, 4)) return false;
    uint8_t* pd = *(uint8_t**)(p + 0x10);
    if (!pd || IsBadReadPtr(pd + 0x310, 4)) return false;
    float h = *(float*)(pd + 0x310);
    if (!std::isfinite(h) || fabsf(h) > 7.0f) return false;
    memcpy(pos, e + 0x60, 12); *heading = h;
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}
void SetMonoCams(const float game[16], const float posed[16]) {
    if (!g_csInit) return; Lock();
    memcpy(g_monoG.m, game, 64); memcpy(g_monoP.m, posed, 64);
    M4* ms[2] = { &g_monoG, &g_monoP }; for (M4* m : ms) { m->m[0][3] = m->m[1][3] = m->m[2][3] = 0; m->m[3][3] = 1; }
    g_monoCamTick = GetTickCount(); Unlock();
}
bool DiagHand(float applyW[3], float liveW[3], float hand[3], float target[3], long* seq) {
    uint8_t* gfx = (uint8_t*)g_playerGfx; if (!g_csInit || !gfx || IsBadReadPtr(gfx + 0x170, 64)) return false;
    const float* m = (const float*)(gfx + 0x170); liveW[0] = m[12]; liveW[1] = m[13]; liveW[2] = m[14];
    Lock(); memcpy(applyW, g_dgW, 12); memcpy(hand, g_dgHand, 12); memcpy(target, g_dgTarget, 12); *seq = g_dgSeq; Unlock();
    return true;
}
bool DiagLiveHand(float out[3]) {   // v231: the right hand bone as it is in the skeleton right now (world)
    uint8_t* skel = (uint8_t*)g_playerSkel; uint8_t* gfx = (uint8_t*)g_playerGfx;
    if (!g_csInit || !skel || !gfx || IsBadReadPtr(skel + 0x20, 16) || IsBadReadPtr(gfx + 0x170, 64)) return false;
    Bone* bones = *(Bone**)(skel + 0x20); int n = *(int*)(skel + 0x2C); int rh = g_slot[12];
    if (!bones || rh < 0 || rh >= n || IsBadReadPtr(&bones[rh], sizeof(Bone))) return false;
    M4 W; memcpy(W.m, gfx + 0x170, 64); W.m[0][3] = W.m[1][3] = W.m[2][3] = 0; W.m[3][3] = 1;
    M4 b; memcpy(b.m, bones[rh].model, 64); b.m[0][3] = b.m[1][3] = b.m[2][3] = 0; b.m[3][3] = 1;
    M4 r = Mul(b, W); for (int k = 0; k < 3; ++k) out[k] = r.m[3][k];
    return std::isfinite(out[0]);
}
float DiagForeStep() { return g_dgForeStep; }
void DiagArm(float pos[10], float rot[10]) { memcpy(pos, g_dgArmPos, 40); memcpy(rot, g_dgArmRot, 40); }
float ArmSmooth() { return g_c.armSmooth; }
void SetArmSmooth(float s) { g_c.armSmooth = s; }
// v231: compare the arm bones in the skeleton now with what the mod wrote: count differing bones, worst bone and size
bool DiagArmLive(int* differ, int* worstBone, float* worstMm) {
    uint8_t* skel = (uint8_t*)g_playerSkel; if (!g_csInit || !skel || IsBadReadPtr(skel + 0x20, 16)) return false;
    Bone* bones = *(Bone**)(skel + 0x20); int n = *(int*)(skel + 0x2C); if (!bones || n <= 0 || n > 512) return false;
    int d = 0, wb = -1; float wm = 0;
    Lock();
    if (g_dgSkel != skel || g_dgN != n) { Unlock(); return false; }
    for (int i = 0; i < n; ++i) { if (!g_dgWritten[i]) continue;
        const float* a = bones[i].model; const float* b = g_dgModel[i]; float m = 0;
        for (int k = 0; k < 3; ++k) m = fmaxf(m, fabsf(a[12 + k] - b[12 + k]));
        for (int k = 0; k < 11; ++k) if (k != 3 && k != 7) m = fmaxf(m, fabsf(a[k] - b[k]) * 0.3f);   // rotation difference as mm at 0.3 m
        if (m > 0.002f) { ++d; if (m > wm) { wm = m; wb = i; } } }
    Unlock();
    *differ = d; *worstBone = wb; *worstMm = wm * 1000.0f; return true;
}
long CamBoneSeq() { return g_camBoneSeq; }
bool BodyClock() { return g_c.bodyClock != 0; }
void SetBodyClock(bool on) { g_c.bodyClock = on ? 1 : 0; }
bool CamBonePos(float out[3]) { if (!g_csInit || g_camBoneSeq == 0) return false; Lock(); memcpy(out, g_camBonePos, 12); Unlock(); return true; }
void Stats(LONG& applied, LONG& shots) { applied = InterlockedExchange(&g_applied, 0); shots = g_shots; }

} // namespace arms
