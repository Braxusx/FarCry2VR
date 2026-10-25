#pragma once
#include "arm_pose.h"

namespace fc2vr::weapon {
using tracking::Vec3;
using tracking::Add;
using tracking::Subtract;
using tracking::Scale;
using tracking::DotProduct;
// Rigid row-vector frame: local +X/+Y/+Z are right/up/back. Positions are game units.
struct Pose {
    Vec3 r{1,0,0}, u{0,1,0}, b{0,0,1}, p{};
    Vec3 Direction(Vec3 v) const { return Add(Add(Scale(r,v.x),Scale(u,v.y)),Scale(b,v.z)); }
    Vec3 Point(Vec3 v) const { return Add(p,Direction(v)); }
    Vec3 LocalDirection(Vec3 v) const { return {DotProduct(v,r),DotProduct(v,u),DotProduct(v,b)}; }
    Vec3 LocalPoint(Vec3 v) const { return LocalDirection(Subtract(v,p)); }
};
inline bool Rigid(const Pose& p) {
    return arm::Finite(p.r) && arm::Finite(p.u) && arm::Finite(p.b) && arm::Finite(p.p)
        && std::fabs(DotProduct(p.r,p.r)-1)<0.02f && std::fabs(DotProduct(p.u,p.u)-1)<0.02f
        && std::fabs(DotProduct(p.b,p.b)-1)<0.02f && std::fabs(DotProduct(p.r,p.u))<0.01f
        && std::fabs(DotProduct(p.r,p.b))<0.01f && std::fabs(DotProduct(p.u,p.b))<0.01f;
}
inline Pose Compose(const Pose& local,const Pose& parent) {
    return {parent.Direction(local.r),parent.Direction(local.u),parent.Direction(local.b),parent.Point(local.p)};
}
inline Pose Relative(const Pose& pose,const Pose& parent) {
    return {parent.LocalDirection(pose.r),parent.LocalDirection(pose.u),parent.LocalDirection(pose.b),parent.LocalPoint(pose.p)};
}
struct SupportKey {
    arm::RigKey rig{};
    std::uintptr_t model=0, palette=0;
    int modelCount=0;
    std::uint32_t definition=0;
    std::uint64_t hold=0;
    bool operator==(const SupportKey& k) const {
        return rig==k.rig && model==k.model && palette==k.palette && modelCount==k.modelCount
            && definition==k.definition && hold==k.hold;
    }
};
// Only the descriptor is persistent. Its world/reference placement is computed
// from the CURRENT physical controller frame, never a previous-frame world point.
class SupportBinding {
    bool have=false;
    SupportKey key{};
    Pose socket{}, rightInAim{};
    authority::Stamp source{};
    std::uint64_t token=0;
public:
    void Clear() { if(have) ++token; have=false; }
    bool Publish(const SupportKey& k,const authority::Stamp& s,const Pose& local,const Pose& right,bool allowed) {
        if (!allowed || !s.sequence || !s.focused || !s.synced || !k.rig.weapon || !Rigid(local) || !Rigid(right)) { Clear(); return false; }
        if (have && key==k && s.generation==source.generation && s.sequence<source.sequence) return false;
        if (!have || !(key==k) || !authority::SameContext(source,s,0) || !authority::SameContext(source,s,1)
            || s.receivedMs-source.receivedMs>authority::MaxAgeMs) {
            socket=local; rightInAim=right; key=k; ++token; have=true;
        }
        rightInAim=right; source=s; return true;
    }
    bool Read(const SupportKey& k,const authority::Stamp& current,std::uint64_t now,Pose& local,Pose& right,std::uint64_t& id) const {
        if (!have || !(key==k) || !authority::Fresh(source,current,now,0) || !authority::Fresh(source,current,now,1)) return false;
        local=socket; right=rightInAim; id=token; return true;
    }
    SupportKey Key() const { return key; }
};
class SupportLatch {
    bool have=false, down=false, held=false;
    std::uint64_t token=0;
    authority::Stamp source{};
public:
    bool Update(bool pressed,bool inRange,bool allowed,std::uint64_t id,const authority::Stamp& s) {
        const bool context=have && token==id && authority::SameContext(source,s,0) && authority::SameContext(source,s,1);
        if (!context || !allowed) { held=false; down=pressed; }
        else {
            if (pressed && !down && inRange) held=true;
            if (!pressed) held=false;
            down=pressed;
        }
        have=true; token=id; source=s; return held;
    }
    bool Down() const { return down; }
};
// Shortest-arc rotation including the antiparallel case. Coincident controller
// points are unavailable, not an arbitrary steering direction.
inline bool FromTo(Vec3 from,Vec3 to,Vec3 hint,Vec3& axis,float& angle) {
    Vec3 a,b;
    if (!arm::Unit(from,a) || !arm::Unit(to,b)) return false;
    const Vec3 c=arm::Cross(a,b);
    const float sn=std::sqrt(DotProduct(c,c)), cs=DotProduct(a,b);
    if (sn>1e-6f) { axis=Scale(c,1/sn); angle=std::atan2(sn,cs); return true; }
    if (cs>0) { axis={1,0,0}; angle=0; return true; }
    hint=Subtract(hint,Scale(a,DotProduct(a,hint)));
    if (!arm::Unit(hint,axis)) {
        const Vec3 fallback=std::fabs(a.x)<0.8f ? Vec3{1,0,0} : Vec3{0,1,0};
        if (!arm::Unit(arm::Cross(a,fallback),axis)) return false;
    }
    angle=arm::Pi; return true;
}
} // namespace fc2vr::weapon
