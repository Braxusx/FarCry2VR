#pragma once
#include "pose_frame.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fc2vr::motion {
using tracking::Vec3;
struct ArmSwingInput {
    bool allowed=false, tracked=false;
    double seconds=0; // pose-sample clock; caller checks physical input freshness separately
    std::uint64_t sequence=0, generation=0, handGeneration[2]{};
    Vec3 head{},hand[2]{},forward{0,0,-1}; // unfiltered OpenXR LOCAL metres; +Y up
};

enum class ArmSwingReason : unsigned {
    NotAllowed,Tracking,InvalidSample,Continuity,Duplicate,Primed,Moving,Pause,
    TooShort,TooSlow,TooSmall,Unbalanced,RigidMotion,Stroke,
    SpeedJump,Clock,Generation,Stale,Replay,Count
};
inline const char* ArmSwingReasonName(ArmSwingReason reason) {
    static const char* const names[]={"notAllowed","tracking","invalid","continuity","duplicate","primed",
        "moving","pause","short","slow","small","unbalanced","rigid","stroke",
        "speedJump","clock","generation","stale","replay"};
    const auto n=unsigned(reason);return n<unsigned(ArmSwingReason::Count)?names[n]:"unknown";
}
struct ArmSwingDiagnostics {
    ArmSwingReason reason=ArmSwingReason::Primed;
    float strokeSeconds=0,leftTravel=0,rightTravel=0,spanVariation=0,leftSpeed=0,rightSpeed=0;
    float rawLeftSpeed=0,rawRightSpeed=0;
    unsigned completedStrokes=0;
    bool opposed=false,active=false;
};

// Requests only the native sprint button. The caller owns forward-stick intent,
// gameplay eligibility, movement and stamina. Filtering below is gesture-only.
class ArmSwingSprint {
    bool have=false,active=false,stroke=false;
    std::uint64_t sequence=0,generation=0,handGeneration[2]{};
    double last=0,lastPoll=0,strokeAt=0,lastMotion=0,lastQualified=0;
    Vec3 previous[2]{},filtered[2]{},start[2]{},axis{};
    float travel[2]{},span=0,spanLow=0,spanHigh=0;
    unsigned strokes=0;
    ArmSwingDiagnostics diagnostic{};
    static bool Finite(Vec3 v) {
        return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
    }
    static float Length(Vec3 v) { return std::sqrt(tracking::DotProduct(v,v)); }
    bool Cancel(ArmSwingReason reason) { Reset();diagnostic.reason=reason;return false; }
    void Start(Vec3 direction,double seconds) {
        axis=tracking::Scale(direction,1/Length(direction));stroke=true;strokeAt=seconds;
        start[0]=filtered[0];start[1]=filtered[1];travel[0]=travel[1]=0;spanLow=spanHigh=span;
    }
public:
    void Reset() { *this={}; }
    ArmSwingDiagnostics Diagnostics() const { return diagnostic; }
    bool Update(const ArmSwingInput& in) {
        using namespace tracking;
        diagnostic={};
        if(!in.allowed) return Cancel(ArmSwingReason::NotAllowed);
        if(!in.tracked) return Cancel(ArmSwingReason::Tracking);
        if(!in.sequence || !std::isfinite(in.seconds) || !Finite(in.head)
            || !Finite(in.hand[0]) || !Finite(in.hand[1]) || !Finite(in.forward))
            return Cancel(ArmSwingReason::InvalidSample);
        Vec3 relative[2]={Subtract(in.hand[0],in.head),Subtract(in.hand[1],in.head)};
        if(Length(relative[0])>1.5f || Length(relative[1])>1.5f)
            return Cancel(ArmSwingReason::InvalidSample);
        auto resetReason=ArmSwingReason::Count;
        if(have) {
            if(generation!=in.generation || handGeneration[0]!=in.handGeneration[0]
                || handGeneration[1]!=in.handGeneration[1]) resetReason=ArmSwingReason::Generation;
            else if(in.seconds<lastPoll) resetReason=ArmSwingReason::Clock;
            else if(in.sequence<sequence) resetReason=ArmSwingReason::Replay;
            else if(in.seconds-last>.1) resetReason=ArmSwingReason::Stale;
        }
        const bool discontinuity=resetReason!=ArmSwingReason::Count;
        if(discontinuity) Reset();
        lastPoll=in.seconds;
        if(have && in.sequence==sequence) {
            diagnostic.reason=ArmSwingReason::Duplicate;diagnostic.active=active;return active;
        }
        if(!have) {
            have=true;sequence=in.sequence;generation=in.generation;
            for(int h=0;h<2;++h) { handGeneration[h]=in.handGeneration[h];previous[h]=filtered[h]=relative[h]; }
            span=Length(Subtract(relative[0],relative[1]));last=in.seconds;
            diagnostic.reason=discontinuity?resetReason:ArmSwingReason::Primed;return false;
        }
        const double dt=in.seconds-last;
        // Distinct pose sequences may share one XR prediction/host timestamp.
        // Do not integrate their displacement twice or renew any stroke timeout.
        if(dt==0) { sequence=in.sequence;diagnostic.reason=ArmSwingReason::Duplicate;diagnostic.active=active;return active; }
        if(dt<0) return Cancel(ArmSwingReason::Clock);
        const float rawSpeed[2]={Length(Subtract(relative[0],previous[0]))/float(dt),
            Length(Subtract(relative[1],previous[1]))/float(dt)};
        // This is a tracking-jump sanity ceiling, not a running-speed target:
        // a 0.5 m, 2.5 Hz arm swing already peaks at 7.85 m/s before vertical
        // travel. Keep room for vigorous 3D motion without admitting teleports.
        if(!std::isfinite(rawSpeed[0]) || !std::isfinite(rawSpeed[1]) || rawSpeed[0]>12.f || rawSpeed[1]>12.f) {
            Cancel(ArmSwingReason::SpeedJump);
            diagnostic.rawLeftSpeed=rawSpeed[0];diagnostic.rawRightSpeed=rawSpeed[1];return false;
        }
        diagnostic.rawLeftSpeed=rawSpeed[0];diagnostic.rawRightSpeed=rawSpeed[1];
        Vec3 velocity[2];float speed[2];
        const float alpha=1-std::exp(-float(dt)/.035f);
        for(int h=0;h<2;++h) {
            const auto next=Add(filtered[h],Scale(Subtract(relative[h],filtered[h]),alpha));
            velocity[h]=Scale(Subtract(next,filtered[h]),1/float(dt));speed[h]=Length(velocity[h]);
            filtered[h]=next;
        }
        // Filter the invariant distance itself: filtering rotating hand vectors
        // would contract their span and could mistake a body turn for a stroke.
        span+=(Length(Subtract(relative[0],relative[1]))-span)*alpha;
        const bool opposed=speed[0]>=.12f && speed[1]>=.12f
            && DotProduct(velocity[0],velocity[1])<-.15f*speed[0]*speed[1];
        diagnostic.leftSpeed=speed[0];diagnostic.rightSpeed=speed[1];diagnostic.opposed=opposed;
        diagnostic.reason=opposed?ArmSwingReason::Moving:ArmSwingReason::Pause;
        if(opposed) lastMotion=in.seconds;
        if(!stroke && opposed) Start(Subtract(velocity[0],velocity[1]),in.seconds);
        if(stroke) {
            const float left=DotProduct(Subtract(filtered[0],start[0]),axis);
            const float right=-DotProduct(Subtract(filtered[1],start[1]),axis);
            travel[0]=std::max(travel[0],left);travel[1]=std::max(travel[1],right);
            spanLow=std::min(spanLow,span);spanHigh=std::max(spanHigh,span);
            const double duration=in.seconds-strokeAt;
            diagnostic.strokeSeconds=float(duration);diagnostic.leftTravel=travel[0];diagnostic.rightTravel=travel[1];
            diagnostic.spanVariation=spanHigh-spanLow;
            // Hold this axis for the entire stroke. A brief opposing sample or
            // an asynchronous hand reversal cannot redefine the frame/reset it.
            const bool reversed=opposed && DotProduct(velocity[0],axis)<-.08f
                && DotProduct(velocity[1],axis)>.08f && travel[0]-left>=.018f && travel[1]-right>=.018f;
            if(reversed) {
                const float shortest=std::min(travel[0],travel[1]),longest=std::max(travel[0],travel[1]);
                auto reason=ArmSwingReason::Stroke;
                if(duration<.14) reason=ArmSwingReason::TooShort;
                else if(duration>.85) reason=ArmSwingReason::TooSlow;
                else if(shortest<.055f) reason=ArmSwingReason::TooSmall;
                else if(shortest<longest*.25f) reason=ArmSwingReason::Unbalanced;
                else if(spanHigh-spanLow<.008f) reason=ArmSwingReason::RigidMotion;
                diagnostic.reason=reason;
                if(reason==ArmSwingReason::Stroke) { ++strokes;lastQualified=in.seconds;if(strokes>=2) active=true; }
                else { strokes=0;if(longest<.04f) active=false; }
                Start(Subtract(velocity[0],velocity[1]),in.seconds);
            } else if(duration>.85) {
                diagnostic.reason=ArmSwingReason::TooSlow;stroke=false;strokes=0;
            }
        }
        // Keep normal reversal pauses; neither tiny shaking nor indefinite
        // opposed movement without qualified cadence can hold sprint latched.
        if(in.seconds-lastMotion>.20 || (active && in.seconds-lastQualified>.75)) {
            active=false;strokes=0;stroke=false;
        }
        for(int h=0;h<2;++h) previous[h]=relative[h];
        last=in.seconds;sequence=in.sequence;
        diagnostic.completedStrokes=strokes;diagnostic.active=active;
        return active;
    }
};
}
