#pragma once
#include "tracking_frame.h"
#include <cmath>
#include <cstdint>

namespace fc2vr::tracking {
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 Subtract(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 Scale(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float DotProduct(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

// Yaw-only game frame: game Z is up; reference -Z is forward.
// All positions here are game units; directions are not scaled.
struct GameBasis {
    Vec3 R{1,0,0}, U{0,0,1}, F{0,1,0}, P{0,0,0};
    Vec3 Direction(Vec3 r) const { return Add(Add(Scale(R,r.x), Scale(U,r.y)), Scale(F,-r.z)); }
    Vec3 Position(Vec3 r) const { return Add(P, Direction(r)); }
    Vec3 ReferenceDirection(Vec3 w) const { return {DotProduct(w,R), DotProduct(w,U), -DotProduct(w,F)}; }
    Vec3 ReferencePosition(Vec3 w) const { return ReferenceDirection(Subtract(w,P)); }
};

struct SteadyInput {
    double nowMs = 0;
    bool eligible = false, turning = false;
    Vec3 playerPosition{0,0,0};
};

// The existing SteadyCamera policy, independent of engine queries and wall clock.
// Build once per sample, never once per eye/consumer.
class CameraSmoother {
    bool have = false;
    Vec3 smLat{}, lastOff{}, lastPos{};
    float smZ = 0, fy = 0, spd = 0;
    double last = 0;
    static float Wrap(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }
public:
    GameBasis Update(GameBasis b, const SteadyInput& input) {
        const float dt = last > 0 ? static_cast<float>((input.nowMs-last)*0.001) : 0;
        last = input.nowMs;
        if (!input.eligible || dt <= 0 || dt >= 0.25f) { have = false; return b; }
        const Vec3 off = Subtract(b.P,input.playerPosition);
        const bool reset = !have || DotProduct(Subtract(off,lastOff),Subtract(off,lastOff)) > 0.25f;
        lastOff = off;
        Vec3 dp = Subtract(input.playerPosition,lastPos); dp.z = 0; lastPos = input.playerPosition;
        float speed = std::sqrt(DotProduct(dp,dp))/dt;
        if (reset || speed > 30) speed = 0;
        spd += (speed-spd)*(1-std::exp(-dt/0.3f));
        const float camYaw = std::atan2(b.F.y,b.F.x), d = Wrap(camYaw-fy);
        if (reset || input.turning || spd < 2 || std::fabs(d) > 0.17f) fy = camYaw;
        else {
            fy = Wrap(fy+d*(1-std::exp(-dt/0.35f)));
            b.F = {std::cos(fy),std::sin(fy),0}; b.R = {b.F.y,-b.F.x,0};
        }
        const Vec3 fwd = Scale(b.F,DotProduct(off,b.F));
        Vec3 lat = Subtract(off,fwd); lat.z = 0;
        if (reset) { smLat = lat; smZ = off.z; have = true; }
        smLat = Add(smLat,Scale(Subtract(lat,smLat),1-std::exp(-dt/0.30f))); smLat.z = 0;
        const float dz = off.z-smZ;
        smZ += dz*(1-std::exp(-dt/(std::fabs(dz) > 0.12f ? 0.08f : 0.30f)));
        Vec3 fh = fwd; fh.z = 0;
        b.P = Add(Add(Add(input.playerPosition,fh),smLat),Vec3{0,0,smZ});
        return b;
    }
};

struct PoseFrame {
    GameBasis raw{}, game{};
    ReferenceFrame reference{};
    float worldScale = 1;   // finite, positive game units/metre (existing config contract)
    std::uint64_t sequence = 0;

    Vec3 ReferencePosition(Vec3 local) const { return Scale(reference.Position(local),worldScale); }
    Vec3 ReferenceDirection(Vec3 local) const { return reference.Direction(local); }
    Vec3 WorldPosition(Vec3 local) const { return game.Position(ReferencePosition(local)); }
    Vec3 WorldDirection(Vec3 local) const { return game.Direction(ReferenceDirection(local)); }
    Vec3 LocalPosition(Vec3 ref) const { return reference.LocalPosition(Scale(ref,1/worldScale)); }
    Vec3 LocalDirection(Vec3 ref) const { return reference.LocalDirection(ref); }
};

class PoseFrameBuilder {
    CameraSmoother smoother;
    PoseFrame published{};
    bool have = false;
public:
    PoseFrame Build(std::uint64_t sequence, const GameBasis& raw, const ReferenceFrame& reference,
                    float worldScale, const SteadyInput& input) {
        if (!have || sequence != published.sequence) {
            published = {raw,smoother.Update(raw,input),reference,worldScale,sequence};
            have = true;
        }
        return published;
    }
};
} // namespace fc2vr::tracking
