#pragma once
// Frozen v230 SteadyCam from a41b3b9:mod/src/main.cpp, unchanged function body.
// Test-only oracle and native-query stubs; never included in the DLL.
#include "common/pose_frame.h"
namespace legacy_v230 {
using V3 = fc2vr::tracking::Vec3;
using GameBasis = fc2vr::tracking::GameBasis;
inline V3 operator+(V3 a,V3 b) { return fc2vr::tracking::Add(a,b); }
inline V3 operator-(V3 a,V3 b) { return fc2vr::tracking::Subtract(a,b); }
inline V3 operator*(V3 a,float s) { return fc2vr::tracking::Scale(a,s); }
inline float Dot(V3 a,V3 b) { return fc2vr::tracking::DotProduct(a,b); }
static fc2vr::tracking::SteadyInput current;
static struct Config { bool steadyCam = true; } g_cfg;
static double NowMs() { return current.nowMs; }
namespace controls {
inline bool NormalControls() { return current.eligible; }
inline int VehicleMode() { return 0; }
inline bool MountedWeapon() { return false; }
}
namespace arms {
inline bool PlayerState(float* pos,float* heading) {
    pos[0] = current.playerPosition.x; pos[1] = current.playerPosition.y; pos[2] = current.playerPosition.z;
    *heading = 0; return true;
}
}
namespace xr {
struct HandState { struct Stick { float x = 0; } stick; };
inline HandState Hand(int) { return {{current.turning ? 1.0f : 0.0f}}; }
}
static void SteadyCam(GameBasis& b) {
    static bool have = false;
    static V3 smLat{}, lastOff{}; static float smZ = 0; static double last = 0;
    double now = NowMs(); float dt = last > 0 ? (float)((now - last) * 0.001) : 0.0f; last = now;
    float pos[3], hd;
    bool ok = g_cfg.steadyCam && controls::NormalControls() && controls::VehicleMode() == 0 && !controls::MountedWeapon() && arms::PlayerState(pos, &hd) && dt > 0 && dt < 0.25f;
    if (!ok) { have = false; return; }
    V3 off = b.P - V3{ pos[0], pos[1], pos[2] };
    bool reset = !have || Dot(off - lastOff, off - lastOff) > 0.25f;
    lastOff = off;
    {   // v211: the sideways sway while running - the game swings its camera's yaw left and right. While moving at running
        // speed with the turning stick at rest, that yaw is smoothed; any stick turn, snap turn, slow walk or a change of
        // more than 10 deg is followed exactly (no lag in turning).
        static float fy = 0; static V3 lastPos{}; static float spd = 0;
        V3 p{ pos[0], pos[1], pos[2] };
        V3 dp = p - lastPos; dp.z = 0; lastPos = p;
        float v = sqrtf(Dot(dp, dp)) / dt; if (reset || v > 30.0f) v = 0;
        spd += (v - spd) * (1.0f - expf(-dt / 0.3f));
        auto wrap = [](float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
        float camYaw = atan2f(b.F.y, b.F.x);
        float d = wrap(camYaw - fy);
        bool turning = fabsf(xr::Hand(1).stick.x) > 0.15f;
        if (reset || turning || spd < 2.0f || fabsf(d) > 0.17f) fy = camYaw;
        else {
            fy = wrap(fy + d * (1.0f - expf(-dt / 0.35f)));
            b.F = { cosf(fy), sinf(fy), 0 }; b.R = { b.F.y, -b.F.x, 0 };
        }
    }
    // (v210: the heading part is gone - the game's heading sign was learned wrongly from a jump after loading, and the
    //  view turned on its own while walking. The view's direction is the game camera's again; only the bob is smoothed.)
    // the position: forward part as is, sideways and vertical smoothed (bob ~2 Hz); big height changes (crouch) followed faster
    V3 fwd = b.F * Dot(off, b.F);
    V3 lat = off - fwd; lat.z = 0;
    if (reset) { smLat = lat; smZ = off.z; have = true; }
    float kl = 1.0f - expf(-dt / 0.30f);
    smLat = smLat + (lat - smLat) * kl; smLat.z = 0;
    float dz = off.z - smZ, kz = 1.0f - expf(-dt / (fabsf(dz) > 0.12f ? 0.08f : 0.30f));
    smZ += dz * kz;
    V3 fh = fwd; fh.z = 0;
    b.P = V3{ pos[0], pos[1], pos[2] } + fh + smLat + V3{ 0, 0, smZ };
}

inline GameBasis Update(GameBasis raw,const fc2vr::tracking::SteadyInput& input) {
    current = input; SteadyCam(raw); return raw;
}
} // namespace legacy_v230
