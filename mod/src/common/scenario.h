#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// A deterministic plan, not a game emulator. Observations come from downstream
// adapters; requesting an input/capture never manufactures an acknowledgement.
namespace fc2vr::scenario {
enum class Case { None,WeaponSwap,TrackingLoss,ArmSweep };
inline Case ParseCase(const char* s) {
    if(!s) return Case::None;
    if(!std::strcmp(s,"weapon-swap")) return Case::WeaponSwap;
    if(!std::strcmp(s,"tracking-loss")) return Case::TrackingLoss;
    if(!std::strcmp(s,"arm-sweep")) return Case::ArmSweep;
    return Case::None;
}
inline const char* Name(Case c) { return c==Case::WeaponSwap?"weapon-swap":c==Case::TrackingLoss?"tracking-loss":c==Case::ArmSweep?"arm-sweep":"none"; }
enum class Pattern { Neutral,Select,Lost,HeldRecovery,WristOut,WristBack,ReachOut,ReachBack };
enum class Check { Ready,Polled,Swap,Lost,Held,Recovered,Pose };
struct Step { const char* name; Pattern pattern; Check check; std::uint64_t dwell; bool capture; };
inline Step Plan(Case c,unsigned i) {
    static constexpr Step swap[]={
        {"baseline",Pattern::Neutral,Check::Ready,500,true},
        {"select-press",Pattern::Select,Check::Polled,200,false},
        {"native-swap",Pattern::Neutral,Check::Swap,500,true},
        {"settled",Pattern::Neutral,Check::Pose,500,true}};
    static constexpr Step loss[]={
        {"baseline",Pattern::Neutral,Check::Ready,500,true},
        {"right-tracking-lost",Pattern::Lost,Check::Lost,500,true},
        {"tracked-trigger-held",Pattern::HeldRecovery,Check::Held,500,true},
        {"neutral-rearm",Pattern::Neutral,Check::Recovered,500,true}};
    static constexpr Step sweep[]={
        {"baseline",Pattern::Neutral,Check::Ready,500,true},
        {"wrist-positive-200",Pattern::WristOut,Check::Pose,2000,true},
        {"wrist-negative-200",Pattern::WristBack,Check::Pose,4000,true},
        {"reach-out",Pattern::ReachOut,Check::Pose,2000,true},
        {"reach-back",Pattern::ReachBack,Check::Pose,2000,true},
        {"neutral",Pattern::Neutral,Check::Pose,500,true}};
    if(c==Case::WeaponSwap && i<4) return swap[i];
    if(c==Case::TrackingLoss && i<4) return loss[i];
    if(c==Case::ArmSweep && i<6) return sweep[i];
    return {"finished",Pattern::Neutral,Check::Ready,0,false};
}
inline unsigned Count(Case c) { return c==Case::None?0:c==Case::ArmSweep?6:4; }
// Sampled checkpoints, not continuous video. No catch-up batch may fabricate
// intermediate GPU observations after a slow readback.
inline unsigned Samples(Case c,unsigned i) {
    return c==Case::ArmSweep&&i>=1&&i<=4?5:c==Case::TrackingLoss&&i>0&&i<4?3:1;
}
struct Vec { float x=0,y=0,z=0; };
struct Quat { float x=0,y=0,z=0,w=1; };
inline bool Finite(Vec v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline bool Unit(Quat q) { const float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;return std::isfinite(n)&&std::fabs(n-1)<0.002f; }
inline Quat Mul(Quat a,Quat b) { return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z}; }
inline Vec Rotate(Quat q,Vec v) { const auto r=Mul(Mul(q,{v.x,v.y,v.z,0}),{-q.x,-q.y,-q.z,q.w});return {r.x,r.y,r.z}; }
struct Pose { Vec p; Quat q; };
struct Hand { Pose grip,aim; bool tracked=true,select=false; float trigger=0; };
inline Hand Fixture(unsigned side,Pattern pattern,float u) {
    Hand h;h.grip.p={side?0.20f:-0.20f,-0.25f,-0.45f};
    float roll=0;
    if(pattern==Pattern::WristOut) roll=200*u;
    if(pattern==Pattern::WristBack) roll=200-400*u;
    if(pattern==Pattern::ReachOut || pattern==Pattern::ReachBack) {
        const float reach=pattern==Pattern::ReachOut?u:1-u;
        // Begin at the preceding wrist sweep endpoint; unwind without a jump.
        roll=pattern==Pattern::ReachOut?-200*(1-u):0;
        h.grip.p.y+=0.22f*reach;h.grip.p.z-=0.22f*reach;
    }
    const float a=roll*0.008726646259971648f;
    h.grip.q={0,0,std::sin(a),std::cos(a)};
    const auto tip=Rotate(h.grip.q,{0,0,-0.03f});
    h.aim=h.grip;h.aim.p.x+=tip.x;h.aim.p.y+=tip.y;h.aim.p.z+=tip.z;
    if(side==1) { h.select=pattern==Pattern::Select;h.tracked=pattern!=Pattern::Lost;h.trigger=(pattern==Pattern::Lost || pattern==Pattern::HeldRecovery)?0.8f:0; }
    return h;
}
// All sequence numbers below refer to the unmodified production source stamps.
struct Observation {
    std::uint64_t sequence=0,ownerEpoch=0,weapon=0,handEpoch=0;
    std::uint64_t appliedSequence=0,appliedOwner=0,appliedMs=0;
    std::uint64_t polledSequence=0,polledOwner=0,polledMs=0,captureTicket=0;
    bool safe=false,rightAuthority=false,rightTracked=false;
    float emittedTrigger=0;
};
enum class State { Idle,Waiting,Running,Complete,Failed,Aborted };
struct Decision {
    bool overrideInput=false,syntheticPose=false,boundary=false,capture=false;
    unsigned step=0,sample=0;std::uint64_t ticket=0,captureId=0,firstSequence=0;
    float progress=0;
    Hand hand[2];const char* label="idle";
};
class Runner {
    Case which=Case::None;State state=State::Idle;
    std::uint64_t began=0,entered=0,first=0,lastNow=0,lastNext=0,baseWeapon=0,baseHand=0;
    std::uint64_t serial=0,baseOwner=0;unsigned index=0,drain=0,sample=0;bool captureRequested=false;
    const char* reason="idle";Decision cached{};
    bool Current(const Observation& o) const { return first && o.sequence>=first; }
    bool Polled(const Observation& o,std::uint64_t now) const { return Current(o)&&o.polledSequence>=first&&o.polledSequence<=o.sequence&&o.polledOwner==o.ownerEpoch&&now>=o.polledMs&&now-o.polledMs<=100; }
    bool Applied(const Observation& o,std::uint64_t now) const { return Current(o)&&o.appliedSequence>=first&&o.appliedSequence<=o.sequence&&o.appliedOwner==o.ownerEpoch&&now>=o.appliedMs&&now-o.appliedMs<=100; }
    bool Satisfied(Check c,const Observation& o,std::uint64_t now) const {
        if(!Current(o) || !o.safe || !o.ownerEpoch || !o.weapon) return false;
        switch(c) {
        case Check::Ready:return o.weapon&&o.rightAuthority&&Polled(o,now)&&Applied(o,now);
        case Check::Polled:return Polled(o,now);
        case Check::Swap:return o.weapon&&o.weapon!=baseWeapon&&o.rightAuthority&&Polled(o,now)&&Applied(o,now);
        case Check::Lost:return o.handEpoch!=baseHand&&!o.rightTracked&&!o.rightAuthority&&Polled(o,now)&&o.emittedTrigger==0;
        case Check::Held:return o.rightTracked&&!o.rightAuthority&&Polled(o,now)&&o.emittedTrigger==0;
        case Check::Recovered:return o.rightTracked&&o.rightAuthority&&Polled(o,now)&&o.emittedTrigger==0&&Applied(o,now);
        case Check::Pose:return o.rightAuthority&&Polled(o,now)&&Applied(o,now);
        }
        return false;
    }
    void End(State s,const char* why) { state=s;reason=why;drain=3;cached={}; }
public:
    bool Start(Case c,std::uint64_t now) {
        if(c==Case::None || state==State::Waiting || state==State::Running || drain) return false;
        which=c;state=State::Waiting;began=entered=lastNow=now;first=lastNext=baseWeapon=baseHand=0;
        index=sample=0;baseOwner=0;captureRequested=false;++serial;reason="waiting-ready";cached={};return true;
    }
    void Abort(const char* why) { if(state==State::Waiting||state==State::Running) End(State::Aborted,why); }
    State Status() const { return state; }
    const char* Reason() const { return reason; }
    Case Which() const { return which; }
    unsigned Index() const { return index; }
    bool Busy() const { return state==State::Waiting||state==State::Running||drain; }
    Decision Tick(std::uint64_t now,std::uint64_t nextSequence,const Observation& o) {
        if(!nextSequence) { Abort("missing-sequence");return {}; }
        if(lastNext==nextSequence) return cached; // never advance twice for one source sample
        if(lastNext && nextSequence<lastNext) Abort("regressing-sequence");
        if(now<lastNow) Abort("regressing-clock");
        lastNext=nextSequence;lastNow=now;
        Decision d;
        if(drain) { d.overrideInput=true;d.boundary=drain==3||drain==1;d.label=reason;--drain;cached=d;return d; }
        if(state!=State::Waiting && state!=State::Running) { cached=d;return d; }
        if(now-began>60000) End(State::Failed,"run-timeout");
        else if(state==State::Running && !o.safe) End(State::Aborted,"unsafe-context");
        else if(state==State::Waiting && now-began>10000) End(State::Failed,"ready-timeout");
        if(drain) { d.overrideInput=d.boundary=true;d.label=reason;--drain;cached=d;return d; }
        d.overrideInput=true;
        if(state==State::Waiting) {
            d.label="waiting-ready";
            if(!o.safe || !o.weapon) { cached=d;return d; }
            state=State::Running;entered=now;first=nextSequence;baseWeapon=o.weapon;baseHand=o.handEpoch;baseOwner=o.ownerEpoch;d.boundary=true;
        }
        const bool allowSwap=which==Case::WeaponSwap&&(index==1||index==2);
        if(!allowSwap&&o.ownerEpoch!=baseOwner) { End(State::Aborted,"unexpected-owner");d.overrideInput=d.boundary=true;d.label=reason;--drain;cached=d;return d; }
        const auto step=Plan(which,index);
        const auto elapsed=now-entered;
        if(elapsed>step.dwell+8000) { End(State::Failed,"step-timeout");d={};d.overrideInput=d.boundary=true;d.label=reason;--drain;cached=d;return d; }
        if(!first) first=nextSequence;
        const float u=step.dwell?std::min(1.0f,float(elapsed)/float(step.dwell)):1;
        d.syntheticPose=true;d.step=index;d.sample=sample;d.ticket=(serial<<16)|(index+1);
        d.captureId=(d.ticket<<8)|(sample+1);d.firstSequence=first;d.label=step.name;d.progress=u;
        d.hand[0]=Fixture(0,step.pattern,u);d.hand[1]=Fixture(1,step.pattern,u);
        const unsigned samples=Samples(which,index);
        const std::uint64_t due=samples==1?step.dwell:step.dwell*sample/(samples-1);
        if(samples>1 && elapsed>due+200 && o.captureTicket!=d.captureId) {
            End(State::Failed,"motion-sample-missed");d={};d.overrideInput=d.boundary=true;d.label=reason;--drain;cached=d;return d;
        }
        const bool observed=Satisfied(step.check,o,now);
        if(observed && elapsed>=due && step.capture) { captureRequested=true;d.capture=true; }
        const bool captured=captureRequested&&o.captureTicket==d.captureId;
        if(captured && sample+1<samples) {
            ++sample;captureRequested=false;d.capture=false; // one acknowledgement per source tick
        } else if(observed && elapsed>=step.dwell && (!step.capture || captured)) {
            if(index==0) baseHand=o.handEpoch; // baseline follows the boundary's normal generation change
            if(step.check==Check::Swap) baseOwner=o.ownerEpoch;
            ++index;entered=now;first=0;sample=0;captureRequested=false;
            if(index==Count(which)) { End(State::Complete,"complete-observed");d={};d.overrideInput=d.boundary=true;d.label=reason;--drain; }
        }
        cached=d;return d;
    }
};
} // namespace fc2vr::scenario
