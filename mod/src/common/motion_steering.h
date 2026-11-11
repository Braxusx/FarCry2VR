#pragma once
#include "pose_frame.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fc2vr::wheel {
using tracking::Vec3;
using tracking::Add;
using tracking::Subtract;
using tracking::Scale;
using tracking::DotProduct;
struct Input {
    bool allowed=false, headTracked=false, tracked[2]{};
    double seconds=0; // pose-sample clock; caller checks physical input freshness separately
    uint64_t sequence=0,generation=0,handGeneration[2]{};
    Vec3 head{},hand[2]{},forward{0,0,-1}; // raw XR LOCAL metres, Y up
    float squeeze[2]{};
    float fullLockDegrees=90;
};
struct Output { bool engaged=false,held[2]{}; float axis=0,degrees=0; };
enum class SteeringReason : unsigned {
    Holding,Acquired,GripRelease,NotAllowed,HeadTracking,HandTracking,InvalidPose,
    Generation,Clock,Stale,Replay,FrozenGeometry,FrozenRate,Rebased,Count
};
inline const char* SteeringReasonName(SteeringReason reason) {
    static const char* const names[]={"holding","acquired","gripRelease","notAllowed","headTracking",
        "handTracking","invalidPose","generation","clock","stale","replay","frozenGeometry","frozenRate","rebased"};
    const auto n=unsigned(reason);return n<unsigned(SteeringReason::Count)?names[n]:"unknown";
}
struct SteeringDiagnostics { SteeringReason reason=SteeringReason::Holding;float deltaDegrees=0; };
// A deliberate two-grip acquisition calibrates the virtual wheel at the hands.
// It is not a native mesh collision test. Two-hand separation controls rotation,
// so moving both hands together cannot steer. One hand may then release/regrip.
// The native vehicle still owns steering response, wheel animation and physics.
class Steering {
    bool armed[2]{},down[2]{},held[2]{},active=false,rebase=false;
    Vec3 center{},right{1,0,0},normal{0,0,1},previous[2]{},pair{};
    float radius=.2f,angle=0;
    double last=0;
    uint64_t sequence=0,generation=0,handGeneration[2]{};
    SteeringDiagnostics diagnostic{};
    static bool Finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
    static float Length(Vec3 v) { return std::sqrt(DotProduct(v,v)); }
    Vec3 Project(Vec3 v) const { return {DotProduct(v,right),v.y,0}; }
    static float Delta(Vec3 before,Vec3 after) {
        return -std::atan2(before.x*after.y-before.y*after.x,DotProduct(before,after));
    }
    Output Result(float lock) const {
        Output o;o.engaged=active;o.held[0]=held[0];o.held[1]=held[1];
        if(active) {
            const float degrees=angle*57.2957795f;
            o.degrees=degrees; // unscaled physical angle for bounded response diagnostics
            const float full=std::isfinite(lock)?std::clamp(lock,30.f,180.f):90.f;
            const float value=std::max(0.f,std::fabs(degrees)-2.f)/(full-2.f);
            o.axis=std::copysign(std::min(value,1.f),degrees);
        }
        return o;
    }
    Output Cancel(SteeringReason reason) { Reset();diagnostic.reason=reason;return {}; }
    Output Freeze(SteeringReason reason,float lock) {
        // A questionable steering measurement does not revoke a tracked grip.
        // Skip its delta and the next valid delta so a returning pose cannot
        // inject a compensating turn after an ignored spike.
        rebase=true;diagnostic.reason=reason;return Result(lock);
    }
public:
    void Reset() { *this=Steering{}; }
    SteeringDiagnostics Diagnostics() const { return diagnostic; }
    Output Update(const Input& in) {
        diagnostic={};
        if(!in.allowed) return Cancel(SteeringReason::NotAllowed);
        if(!in.headTracked) return Cancel(SteeringReason::HeadTracking);
        if(!Finite(in.head) || !Finite(in.forward) || !std::isfinite(in.seconds) || !in.sequence)
            return Cancel(SteeringReason::InvalidPose);
        if(sequence && in.sequence<sequence) return Cancel(SteeringReason::Replay);
        for(int i=0;i<2;++i) if(held[i]) {
            if(!in.tracked[i]) return Cancel(SteeringReason::HandTracking);
            if(!Finite(in.hand[i]) || !std::isfinite(in.squeeze[i])) return Cancel(SteeringReason::InvalidPose);
        }
        auto resetReason=SteeringReason::Holding;
        if(sequence) {
            if(generation!=in.generation || handGeneration[0]!=in.handGeneration[0]
                || handGeneration[1]!=in.handGeneration[1]) resetReason=SteeringReason::Generation;
            else if(in.seconds<last) resetReason=SteeringReason::Clock;
            else if(in.seconds-last>.1) resetReason=SteeringReason::Stale;
        }
        if(resetReason!=SteeringReason::Holding) { Reset();diagnostic.reason=resetReason; }
        const bool wasHeld[2]={held[0],held[1]};
        // A real button release remains authoritative even if the pose clock
        // or sequence is tied. Only integration is skipped for duplicate poses.
        for(int i=0;i<2;++i) if(held[i] && in.squeeze[i]<.35f) {
            held[i]=down[i]=false;armed[i]=true;diagnostic.reason=SteeringReason::GripRelease;
        }
        if(active && !held[0] && !held[1]) { active=false;angle=0;rebase=false; }
        if(sequence && in.sequence<=sequence) return Result(in.fullLockDegrees);
        // Ignore an untimed pose without moving the integration baseline. Input
        // revocation above still cancels immediately, including on a tied clock.
        if(sequence && in.seconds==last) { sequence=in.sequence;return Result(in.fullLockDegrees); }
        const double dt=sequence?in.seconds-last:0;
        generation=in.generation;handGeneration[0]=in.handGeneration[0];handGeneration[1]=in.handGeneration[1];
        sequence=in.sequence;last=in.seconds;
        for(int i=0;i<2;++i) {
            if(!in.tracked[i] || !Finite(in.hand[i]) || !std::isfinite(in.squeeze[i])) {
                held[i]=down[i]=armed[i]=false;continue;
            }
            if(in.squeeze[i]<.35f) { armed[i]=true;down[i]=false;held[i]=false; }
            else if(in.squeeze[i]>.6f) down[i]=true;
        }
        if(active && !held[0] && !held[1]) { active=false;angle=0; }
        if(!active) {
            if(!down[0] || !down[1] || !armed[0] || !armed[1]) return {};
            const float len=std::hypot(in.forward.x,in.forward.z);
            if(len<.2f) return {};
            right={-in.forward.z/len,0,in.forward.x/len};
            normal={in.forward.x/len,0,in.forward.z/len};
            center=Add(Scale(in.hand[0],.5f),Scale(in.hand[1],.5f));
            const Vec3 reach=Subtract(center,in.head),span=Subtract(in.hand[1],in.hand[0]);
            const float front=DotProduct(reach,normal);
            pair=Project(span);radius=Length(pair)*.5f;
            if(front<.08f || front>1.1f || reach.y<-.95f || reach.y>.25f
                || radius<.07f || radius>.4f || std::fabs(DotProduct(span,normal))>.30f) return {};
            active=held[0]=held[1]=true;angle=0;rebase=false;diagnostic.reason=SteeringReason::Acquired;
            for(int i=0;i<2;++i) previous[i]=Project(Subtract(in.hand[i],center));
            return Result(in.fullLockDegrees);
        }
        Vec3 radial[2];bool withinReach[2]{};
        for(int i=0;i<2;++i) {
            const Vec3 offset=Subtract(in.hand[i],center);radial[i]=Project(offset);
            const float r=Length(radial[i]);
            withinReach[i]=in.tracked[i] && Finite(in.hand[i]) && r>.04f
                && std::fabs(r-radius)<.18f && std::fabs(DotProduct(offset,normal))<.3f;
            // Reach gates acquire/rejoin a hand; they do not release an already
            // latched grip when the hands translate or the player leans.
            if(withinReach[i] && !held[i] && down[i] && armed[i]) { held[i]=true;previous[i]=radial[i]; }
        }
        float delta=0;
        if(held[0] && held[1]) {
            const Vec3 current=Project(Subtract(in.hand[1],in.hand[0]));
            const float length=Length(current);
            if(!Finite(current) || !std::isfinite(length) || length<.14f)
                return Freeze(SteeringReason::FrozenGeometry,in.fullLockDegrees);
            if(!rebase && wasHeld[0] && wasHeld[1]) delta=Delta(pair,current);
            pair=current;center=Add(Scale(in.hand[0],.5f),Scale(in.hand[1],.5f));
            for(int i=0;i<2;++i) radial[i]=Project(Subtract(in.hand[i],center));
        } else if(held[0] || held[1]) {
            const int side=held[1]?1:0;
            if(!withinReach[side]) return Freeze(SteeringReason::FrozenGeometry,in.fullLockDegrees);
            if(!rebase && wasHeld[side]) delta=Delta(previous[side],radial[side]);
        } else return Cancel(SteeringReason::GripRelease);
        diagnostic.deltaDegrees=delta*57.2957795f;
        if(!std::isfinite(delta)) return Freeze(SteeringReason::FrozenGeometry,in.fullLockDegrees);
        if(dt<=0) return Cancel(SteeringReason::Clock);
        if(std::fabs(delta)>float(dt)*12.f+.03f) return Freeze(SteeringReason::FrozenRate,in.fullLockDegrees);
        if(rebase) { rebase=false;diagnostic.reason=SteeringReason::Rebased; }
        angle=std::clamp(angle+delta,-4.712389f,4.712389f);
        previous[0]=radial[0];previous[1]=radial[1];
        return Result(in.fullLockDegrees);
    }
};
} // namespace fc2vr::wheel
