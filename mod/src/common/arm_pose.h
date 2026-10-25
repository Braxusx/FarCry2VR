#pragma once
#include "pose_authority.h"
#include "pose_frame.h"
#include <cmath>
#include <cstdint>

namespace fc2vr::arm {
using tracking::Vec3;
constexpr float Pi = 3.14159265358979323846f;
constexpr float Tau = 2*Pi;
inline bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline bool Unit(Vec3 v, Vec3& out) {
    const float n = tracking::DotProduct(v,v);
    if (!Finite(v) || !std::isfinite(n) || n < 1e-10f) return false;
    out = tracking::Scale(v,1/std::sqrt(n)); return true;
}
inline float Wrap(float a) { return std::remainder(a,Tau); }
// Reject a near-axis projection: atan2(0,0) is not evidence of zero twist.
inline bool ProjectedAngle(Vec3 a, Vec3 b, Vec3 axis, float& out) {
    Vec3 u;
    if (!Unit(axis,u) || !Finite(a) || !Finite(b)) return false;
    a = tracking::Subtract(a,tracking::Scale(u,tracking::DotProduct(a,u)));
    b = tracking::Subtract(b,tracking::Scale(u,tracking::DotProduct(b,u)));
    Vec3 x,y;
    if (!Unit(a,x) || !Unit(b,y)) return false;
    out = std::atan2(tracking::DotProduct(Cross(x,y),u),tracking::DotProduct(x,y));
    return std::isfinite(out);
}
// Observed identities, not a native lifetime/ABA guarantee. No native dereferences.
struct RigKey {
    std::uintptr_t pawn=0, skeleton=0, palette=0, weapon=0;
    int count=0, upper=-1, fore=-1, hand=-1;
    unsigned mode=0;
    bool operator==(const RigKey& k) const {
        return pawn==k.pawn && skeleton==k.skeleton && palette==k.palette && weapon==k.weapon
            && count==k.count && upper==k.upper && fore==k.fore && hand==k.hand && mode==k.mode;
    }
};
class TwistHistory {
    RigKey key{};
    authority::Stamp source{};
    bool bound=false, have=false;
    float previous=0, continuous=0;
public:
    int hinge=-1; // selected once per observed rig/context, not once per animation call
    void Clear() { bound=have=false; hinge=-1; source={}; }
    bool Bind(const RigKey& k, const authority::Stamp& s, int side) {
        if (bound && key==k && s.generation==source.generation && s.sequence<source.sequence) return have;
        const bool same = bound && key==k && authority::SameContext(source,s,side)
            && s.receivedMs-source.receivedMs <= authority::MaxAgeMs;
        if (!same) { Clear(); key=k; source=s; bound=true; }
        return same && have;
    }
    bool Resolve(float measured, const RigKey& k, const authority::Stamp& s, int side, bool valid, float& out) {
        if (!valid || !std::isfinite(measured) || !s.sequence || !s.focused || !s.synced) { Clear(); return false; }
        if (bound && key==k && s.generation==source.generation && s.sequence<source.sequence) return false;
        const bool same = Bind(k,s,side);
        if (same && s.sequence == source.sequence) { out=continuous; return true; }
        const float wrapped = Wrap(measured);
        // One turn in either direction is enough for a fractional arm correction.
        // Saturate with anti-windup; never accumulate unlimited anatomical winding.
        continuous = same ? continuous+Wrap(wrapped-previous) : wrapped;
        continuous = continuous > Tau ? Tau : continuous < -Tau ? -Tau : continuous;
        previous=wrapped; source=s; key=k; have=true; out=continuous; return true;
    }
};
} // namespace fc2vr::arm
