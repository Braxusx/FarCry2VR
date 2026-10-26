#include "common/scenario.h"
#include "common/scenario_capture.h"
#include "common/pose_authority.h"
#include <cstdio>
#include <limits>
#include <string>
using namespace fc2vr::scenario;
namespace {
int checks=0,failures=0;
void Verify(bool b,const char* why) { ++checks;if(!b) { ++failures;std::fprintf(stderr,"FAIL: %s\n",why); } }
struct Harness {
    Runner r;Observation o;std::uint64_t now=1000,seq=1;bool doSwap=true,doPoll=true,doApply=true,doCapture=true,pressed=false,wasTracked=false;
    unsigned captures=0,neutralDrains=0;fc2vr::authority::RearmGate gate;
    Harness(Case c) { o.safe=true;o.ownerEpoch=1;o.weapon=10;r.Start(c,now); }
    Decision Tick() {
        auto d=r.Tick(now,seq,o);
        if(d.boundary) { gate.Clear();wasTracked=false;++o.handEpoch; }
        const auto& h=d.hand[1];
        const bool tracked=d.syntheticPose&&h.tracked;
        if(wasTracked&&!tracked) ++o.handEpoch;
        wasTracked=tracked;o.sequence=seq;
        o.rightTracked=tracked;o.rightAuthority=gate.Update(tracked,h.trigger<=0.15f);
        if(d.syntheticPose&&h.select) pressed=true;
        if(d.syntheticPose&&!h.select&&pressed) { if(doSwap) { o.weapon=20;++o.ownerEpoch; }pressed=false; }
        if(doPoll) { o.polledSequence=seq;o.polledOwner=o.ownerEpoch;o.polledMs=now;o.emittedTrigger=o.rightAuthority?h.trigger:0; }
        if(doApply&&tracked) { o.appliedSequence=seq;o.appliedOwner=o.ownerEpoch;o.appliedMs=now; }
        if(d.capture&&doCapture) { if(o.captureTicket!=d.ticket) ++captures;o.captureTicket=d.ticket; }
        if(d.overrideInput&&!d.syntheticPose) ++neutralDrains;
        now+=20;++seq;return d;
    }
    void Run() { for(unsigned i=0;i<3500&&r.Busy();++i) Tick(); }
};
void Parse() {
    Verify(ParseCase(nullptr)==Case::None && ParseCase("unknown")==Case::None,"no implicit/default test selection");
    Verify(ParseCase("arm-sweep ")==Case::None,"strict case names");
    for(const auto c:{Case::WeaponSwap,Case::TrackingLoss,Case::ArmSweep}) {
        Verify(ParseCase(Name(c))==c,"known case roundtrip");
        for(unsigned i=0;i<Count(c);++i) Verify(Plan(c,i).dwell>0 && Plan(c,i).name[0],"every step named and bounded");
        Verify(!Plan(c,Count(c)).capture,"out of range cannot capture");
    }
}
void Swap() {
    Harness good(Case::WeaponSwap);good.Run();Verify(good.r.Status()==State::Complete&&good.captures==3,"observed swap plus matched captures completes");
    Harness missing(Case::WeaponSwap);missing.doSwap=false;missing.Run();Verify(missing.r.Status()==State::Failed,"button request alone never proves native swap");
    Harness partial(Case::WeaponSwap);partial.doApply=false;partial.Run();Verify(partial.r.Status()==State::Failed,"no applied pose cannot green baseline");
    Harness noPoll(Case::WeaponSwap);noPoll.doPoll=false;noPoll.Run();Verify(noPoll.r.Status()==State::Failed,"no XInput poll cannot green baseline");
    Harness noCapture(Case::WeaponSwap);noCapture.doCapture=false;noCapture.Run();Verify(noCapture.r.Status()==State::Failed,"no images cannot green baseline");
}
void Loss() {
    Harness h(Case::TrackingLoss);bool lost=false,held=false,rearmed=false;
    for(unsigned i=0;i<1500&&h.r.Busy();++i) {
        const auto d=h.Tick();if(!d.syntheticPose) continue;
        if(d.step==1) { lost=true;Verify(!h.o.rightAuthority&&h.o.emittedTrigger==0,"untracked held input remains physically revoked"); }
        if(d.step==2) { held=true;Verify(!h.o.rightAuthority&&h.o.emittedTrigger==0,"tracked held input cannot rearm"); }
        if(d.step==3) { rearmed=true;Verify(h.o.rightAuthority&&h.o.emittedTrigger==0,"neutral recovery rearms without firing"); }
    }
    Verify(lost&&held&&rearmed&&h.r.Status()==State::Complete&&h.captures==4,"entire loss/recovery path observed");
    Harness bad(Case::TrackingLoss);for(int i=0;i<40;++i) bad.Tick();bad.o.safe=false;bad.Run();Verify(bad.r.Status()==State::Aborted,"leaving safe gameplay aborts");
}
void Sweep() {
    Harness h(Case::ArmSweep);h.Run();Verify(h.r.Status()==State::Complete&&h.captures==6,"full deterministic sweep completes with modelled downstream observations");
    for(const auto p:{Pattern::Neutral,Pattern::WristOut,Pattern::WristBack,Pattern::ReachOut,Pattern::ReachBack}) for(int i=0;i<=1000;++i) for(unsigned s=0;s<2;++s) {
        const auto hand=Fixture(s,p,float(i)/1000);Verify(Finite(hand.grip.p)&&Unit(hand.grip.q),"sweep finite and unit quaternion");
        const float x=hand.aim.p.x-hand.grip.p.x,y=hand.aim.p.y-hand.grip.p.y,z=hand.aim.p.z-hand.grip.p.z;
        Verify(std::fabs(std::sqrt(x*x+y*y+z*z)-0.03f)<1e-5f,"aim remains rigidly offset from grip");
        Verify(hand.trigger==0&&!hand.select,"sweeps never request fire/select");
    }
    auto a=Fixture(1,Pattern::WristOut,1),b=Fixture(1,Pattern::WristBack,0);
    Verify(std::fabs(a.grip.q.z-b.grip.q.z)<1e-6f,"positive and reverse sweep join without a quaternion jump");
    a=Fixture(1,Pattern::WristBack,1);b=Fixture(1,Pattern::ReachOut,0);
    Verify(std::fabs(a.grip.q.w-b.grip.q.w)<1e-6f,"wrist and elbow/reach phases join continuously");
}
void Lifecycle() {
    Harness h(Case::ArmSweep);h.Tick();const auto d=h.r.Tick(h.now,h.seq,h.o),duplicate=h.r.Tick(h.now+100,h.seq,h.o);
    Verify(d.ticket==duplicate.ticket&&d.hand[1].grip.q.w==duplicate.hand[1].grip.q.w,"duplicate input sample never advances stimulus");
    h.r.Abort("operator-stop");h.Run();Verify(h.r.Status()==State::Aborted&&h.neutralDrains>=3,"abort drains neutral and does not synthesize release gestures");
    Harness owner(Case::ArmSweep);for(int i=0;i<40;++i) owner.Tick();++owner.o.ownerEpoch;owner.Run();Verify(owner.r.Status()==State::Aborted,"unexpected owner changes abort sweep");
    Harness time(Case::TrackingLoss);time.Tick();time.now-=21;time.Tick();Verify(time.r.Status()==State::Aborted,"regressing time aborts");
    Harness missing(Case::TrackingLoss);missing.o.safe=false;missing.Run();Verify(missing.r.Status()==State::Failed,"missing positive-control gameplay times out");
    Runner idle;Verify(!idle.Busy()&&idle.Status()==State::Idle&&!idle.Start(Case::None,1),"idle/default is nonmutating");
}
void Pair() {
    EyePair p;Verify(!p.Add(0,1,10,20,30,true),"one eye is incomplete");Verify(!p.Add(1,1,11,20,30,true),"different frames cannot form a pair");
    Verify(p.Add(0,1,11,20,30,true),"same-frame pair acknowledged");p.Clear();
    Verify(!p.Add(0,2,12,21,31,true)&&!p.Add(1,2,12,22,31,true),"different source sequences cannot pair");
    Verify(!p.Add(0,3,13,23,32,true)&&!p.Add(1,3,13,23,33,true),"different owners cannot pair");
    Verify(!p.Add(2,4,14,24,34,true)&&!p.Add(0,4,14,24,34,false),"failed/invalid capture cannot acknowledge");
}
void Bitmap() {
    std::vector<std::uint8_t> out;const std::uint8_t px[16]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    Verify(Bmp(px,sizeof px,2,2,8,Pixels::Rgba8,out)&&out.size()==70,"bounded RGBA bitmap encode");
    Verify(out[0]=='B'&&out[1]=='M'&&out[54]==255&&out[56]==0&&out[62]==0&&out[64]==255,"BMP bottom-up and channel mapping");
    Verify(Bmp(px,sizeof px,2,2,8,Pixels::Bgra8,out)&&out[54]==0&&out[56]==255,"BGRA channel mapping");
    Verify(!Bmp(nullptr,16,2,2,8,Pixels::Rgba8,out)&&out.empty(),"null image rejected without stale bytes");
    Verify(!Bmp(px,15,2,2,8,Pixels::Rgba8,out)&&!Bmp(px,16,4097,2,8,Pixels::Rgba8,out),"truncated and excessive images refused");
    Verify(!Bmp(px,16,2,2,0,Pixels::Rgba8,out)&&!Bmp(px,16,2,2,8,Pixels::Unsupported,out),"bad stride and unsupported formats refused");
    float hdr[4]={1,0,0,1};Verify(Bmp(reinterpret_cast<const std::uint8_t*>(hdr),sizeof hdr,1,1,16,Pixels::Rgba32f,out)&&out[56]>180,"HDR tone-map fixture");
    hdr[0]=std::numeric_limits<float>::quiet_NaN();Verify(!Bmp(reinterpret_cast<const std::uint8_t*>(hdr),sizeof hdr,1,1,16,Pixels::Rgba32f,out)&&out.empty(),"nonfinite HDR rejected");
    Verify(Half(0x3c00)==1&&Half(0xbc00)==-1&&Half(1)>0&&!std::isfinite(Half(0x7c00)),"half-float controls");
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const std::string group=argv[1];
    if(group=="parse") Parse();else if(group=="swap") Swap();else if(group=="loss") Loss();else if(group=="sweep") Sweep();else if(group=="lifecycle") Lifecycle();else if(group=="pair") Pair();else if(group=="bitmap") Bitmap();else return 2;
    std::printf("%s: %d checks, %d failures (environment=harness; no game acceptance)\n",argv[1],checks,failures);return failures?1:0;
}
