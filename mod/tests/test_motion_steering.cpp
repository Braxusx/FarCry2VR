#include "common/motion_steering.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace fc2vr::wheel;
static unsigned checks=0;
static void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
static bool Near(float a,float b,float tolerance=.0002f) { return std::fabs(a-b)<tolerance; }
static Vec3 Yaw(Vec3 p,float a) {
    return {std::cos(a)*p.x+std::sin(a)*p.z,p.y,-std::sin(a)*p.x+std::cos(a)*p.z};
}
static Input Sample(int frame,float hz=90,float degrees=0,float yaw=0) {
    Input in;in.allowed=in.headTracked=in.tracked[0]=in.tracked[1]=true;
    in.sequence=uint64_t(frame)+1;in.generation=3;in.handGeneration[0]=7;in.handGeneration[1]=9;
    in.seconds=10+frame/double(hz);in.head={0,1.7f,0};in.forward=Yaw({0,0,-1},yaw);
    const float a=degrees*.01745329252f;
    for(int side=0;side<2;++side) {
        const float x=side?.23f:-.23f;
        in.hand[side]=Yaw({x*std::cos(a),1.15f-x*std::sin(a),-.4f},yaw);
        in.squeeze[side]=1;
    }
    return in;
}
static Output Acquire(Steering& steering,float hz=90,float yaw=0) {
    for(int frame=0;frame<2;++frame) {
        auto in=Sample(frame,hz,0,yaw);in.squeeze[0]=in.squeeze[1]=0;
        Check(!steering.Update(in).engaged,"neutral hands must not steer");
    }
    const auto out=steering.Update(Sample(2,hz,0,yaw));
    Check(out.engaged && out.held[0] && out.held[1] && Near(out.axis,0),"deliberate bilateral grip must acquire at neutral");
    return out;
}
static float Expected(float degrees,float full=90) {
    return std::copysign(std::min(1.f,std::max(0.f,std::fabs(degrees)-2)/(full-2)),degrees);
}
int main() {
    for(float hz:{60.f,90.f,120.f}) for(int mode=0;mode<3;++mode) {
        Steering steering;Acquire(steering,hz);
        for(int i=3;i<int(hz*2);++i) {
            auto in=Sample(i,hz);Vec3 shift{};
            if(mode==0) shift={.04f*std::sin(i*.7f),.05f*std::sin(i*.9f),.06f*std::sin(i*.6f)};
            if(mode==1) shift={.20f,.15f,-.45f}; // common lean beyond the original reach plane
            if(mode==2) shift={i%2?.22f:0.f,0,0}; // abrupt common lateral displacement
            for(auto& hand:in.hand) hand=Add(hand,shift);
            const auto out=steering.Update(in);
            Check(out.engaged && out.held[0] && out.held[1],"valid held grips stay latched through common bumps and leaning");
            Check(Near(out.axis,0),"common displacement must not turn a latched wheel");
        }
    }
    for(float hz:{60.f,90.f,120.f}) {
        Steering steering;Acquire(steering,hz);
        auto out=steering.Update(Sample(3,hz,25));
        Check(out.engaged && out.held[0] && out.held[1] && Near(out.axis,0),"angular spike freezes output without dropping valid held grips");
        Check(steering.Diagnostics().reason==SteeringReason::FrozenRate,"angular outlier has a freeze diagnostic");
        out=steering.Update(Sample(4,hz));
        Check(out.engaged && Near(out.axis,0) && steering.Diagnostics().reason==SteeringReason::Rebased,
            "return from ignored spike rebases without countersteering");
        out=steering.Update(Sample(5,hz,4));
        Check(out.engaged && Near(out.axis,Expected(4)),"ordinary steering resumes after a spike without regripping");
    }
    for(float hz:{60.f,90.f,120.f}) {
        Steering steering;Acquire(steering,hz);
        for(int frame=3;frame<6;++frame) {
            auto in=Sample(frame,hz);in.hand[0].x=-.03f;in.hand[1].x=.03f;
            const auto out=steering.Update(in);
            Check(out.engaged && out.held[0] && out.held[1] && Near(out.axis,0),"ambiguous short hand span retains grip and last steering angle");
            Check(steering.Diagnostics().reason==SteeringReason::FrozenGeometry,"degenerate span reports frozen geometry");
        }
        auto out=steering.Update(Sample(6,hz,20));
        Check(out.engaged && Near(out.axis,0) && steering.Diagnostics().reason==SteeringReason::Rebased,
            "recovering valid span establishes a baseline instead of applying an unmeasured turn");
        out=steering.Update(Sample(7,hz,24));
        Check(out.engaged && Near(out.axis,Expected(4)),"relative steering resumes from recovered span");
    }
    for(float hz:{60.f,90.f,120.f}) for(int kept:{0,1}) {
        Steering steering;Acquire(steering,hz);
        for(int frame=3;frame<7;++frame) {
            auto in=Sample(frame,hz);in.squeeze[1-kept]=0;
            if(frame>3) in.hand[kept].z-=.45f;
            const auto out=steering.Update(in);
            Check(out.engaged && out.held[kept] && !out.held[1-kept] && Near(out.axis,0),
                "single tracked grip remains latched while leaning outside the integration geometry");
            if(frame>3) Check(steering.Diagnostics().reason==SteeringReason::FrozenGeometry,"single-hand lean freezes its unreliable angle");
        }
        auto in=Sample(7,hz);in.squeeze[1-kept]=0;auto out=steering.Update(in);
        Check(out.engaged && Near(out.axis,0) && steering.Diagnostics().reason==SteeringReason::Rebased,"single-hand return rebases without a jump");
        in=Sample(8,hz,4);in.squeeze[1-kept]=0;out=steering.Update(in);
        Check(out.engaged && Near(out.axis,Expected(4)),"single-hand steering resumes while grip stays down");
    }
    for(int reason=0;reason<8;++reason) for(bool tied:{false,true}) {
        Steering steering;Acquire(steering);steering.Update(Sample(3,90,25));
        auto in=Sample(tied?3:4);
        if(reason==0) in.squeeze[0]=in.squeeze[1]=0;
        if(reason==1) in.allowed=false;
        if(reason==2) in.headTracked=false;
        if(reason==3) in.tracked[0]=false;
        if(reason==4) ++in.generation;
        if(reason==5) in.seconds+=.2;
        if(reason==6) in.head.x=std::numeric_limits<float>::quiet_NaN();
        if(reason==7) in.sequence=2;
        const auto out=steering.Update(in);
        Check(!out.engaged && !out.held[0] && !out.held[1] && Near(out.axis,0),"frozen wheel still immediately obeys real grip/authority revocation");
        const SteeringReason expected[]={SteeringReason::GripRelease,SteeringReason::NotAllowed,SteeringReason::HeadTracking,
            SteeringReason::HandTracking,SteeringReason::Generation,SteeringReason::Stale,SteeringReason::InvalidPose,SteeringReason::Replay};
        Check(steering.Diagnostics().reason==expected[reason],"release diagnostic distinguishes authority loss from frozen measurements");
        if(reason!=0) {
            auto recover=Sample(8);recover.seconds=std::max(recover.seconds,in.seconds)+1./90.;recover.generation=in.generation;
            Check(!steering.Update(recover).engaged,"authority recovery with grips down still requires a fresh neutral rearm");
        }
    }
    // Real-style 90 Hz XR samples plus a 15.625 ms host tick, integer-ms
    // GetTickCount64 reporting, phase offset and small frame-time variation.
    // Compare with using the XR pose timestamp for exactly the same hand path.
    for(bool xrClock: {false,true}) {
        Steering steering;
        std::int64_t elapsedNs=7000000;
        const std::int64_t jitter[]={0,350000,-350000,150000,-150000};
        double lastClock=-1;unsigned repeated=0;bool acquired=false;float acquiredAngle=0;
        for(int i=0;i<270;++i) {
            if(i) elapsedNs+=11111111+jitter[i%5];
            const float t=float(double(elapsedNs-7000000)*1e-9);
            const float degrees=std::clamp((t-2/90.f)*45.f,0.f,90.f);
            auto in=Sample(i,90,degrees);
            if(i<2) in.squeeze[0]=in.squeeze[1]=0;
            const auto hostMs=((elapsedNs/15625000)*15625000)/1000000;
            in.seconds=xrClock ? 9000+double(elapsedNs)*1e-9 : 2000+double(hostMs)*.001;
            if(in.seconds==lastClock) ++repeated;
            lastClock=in.seconds;
            const auto out=steering.Update(in);
            if(out.engaged && !acquired) { acquired=true;acquiredAngle=degrees; }
            if(i>5) {
                Check(out.engaged,"coarse adapter clock must not release an acquired steering wheel");
                Check(Near(out.axis,Expected(degrees-acquiredAngle),.02f),"wheel must follow the pose path under both time sources");
            }
        }
        Check(acquired,"clock regression must exercise acquired steering");
        Check(xrClock ? repeated==0 : repeated>50,"clock regression must actually contain coarse repeated timestamps");
    }
    for(float hz: {60.f,90.f,120.f}) for(float yaw: {-2.f,0.f,1.2f}) for(float sign: {-1.f,1.f}) {
        Steering steering;Acquire(steering,hz,yaw);
        for(int i=3;i<=int(hz)+2;++i) {
            float degrees=sign*(i-2)/hz*120;
            auto out=steering.Update(Sample(i,hz,degrees,yaw));
            Check(out.engaged && out.held[0] && out.held[1],"two-hand rotation must retain wheel");
            Check(Near(out.axis,Expected(degrees)),"clockwise must steer right, counterclockwise left, with full lock clamp");
        }
        // Return through the atan2 branch without a sign jump.
        for(int i=1;i<=int(hz);++i) {
            float degrees=sign*(120-i/hz*120);
            auto out=steering.Update(Sample(int(hz)+2+i,hz,degrees,yaw));
            Check(out.engaged && Near(out.axis,Expected(degrees)),"return to centre must preserve wheel angle");
        }
    }
    for(int revocation=0;revocation<3;++revocation) {
        Steering steering;Acquire(steering);auto in=Sample(2);++in.sequence;
        Check(steering.Update(in).engaged,"new sequence with tied time must retain the wheel");
        if(revocation==0) in.allowed=false;
        if(revocation==1) in.tracked[0]=false;
        if(revocation==2) ++in.handGeneration[0];
        ++in.sequence;
        Check(!steering.Update(in).engaged,"same-time context/tracking/generation loss must still cancel the wheel");
    }
    {
        Steering steering;
        for(int i=0;i<30;++i) Check(!steering.Update(Sample(i)).engaged,"held grips on driver entry must require neutral");
        auto in=Sample(30);in.squeeze[0]=0;steering.Update(in);
        in=Sample(31);Check(!steering.Update(in).engaged,"only one neutral hand cannot arm wheel acquisition");
        in=Sample(32);in.squeeze[1]=0;steering.Update(in);
        in=Sample(33);Check(steering.Update(in).engaged,"both hands neutral then grip may acquire");
    }
    {
        Steering steering;Acquire(steering);
        for(int i=3;i<600;++i) {
            auto in=Sample(i);Vec3 shift{.2f*std::sin(i*.01f),.05f*std::sin(i*.03f),.1f*std::sin(i*.012f)};
            for(auto& h:in.hand) h=Add(h,shift);
            auto out=steering.Update(in);
            Check(out.engaged && Near(out.axis,0),"common hand translation must not steer");
        }
    }
    for(int kept: {0,1}) {
        Steering steering;Acquire(steering);
        for(int i=3;i<93;++i) {
            auto in=Sample(i,90,(i-2)*.5f);in.squeeze[1-kept]=0;
            auto out=steering.Update(in);
            Check(out.engaged && out.held[kept] && !out.held[1-kept],"either single hand may continue holding");
            Check(Near(out.axis,Expected((i-2)*.5f)),"single hand rotation must steer in the same direction");
        }
        auto regrip=Sample(93,90,45); // released hand rejoins at a different angle
        auto other=Sample(93,90,20);regrip.hand[1-kept]=other.hand[1-kept];
        auto out=steering.Update(regrip);
        Check(out.engaged && out.held[0] && out.held[1] && Near(out.axis,Expected(45)),"regrip must not jump the steering axis");
        ++regrip.sequence;regrip.seconds+=1./90.;out=steering.Update(regrip);
        Check(out.engaged && Near(out.axis,Expected(45)),"stationary regrip must remain stable");
        ++regrip.sequence;regrip.seconds+=1./90.;regrip.squeeze[0]=regrip.squeeze[1]=0;
        out=steering.Update(regrip);Check(!out.engaged && !out.held[0] && !out.held[1] && Near(out.axis,0),"release both hands must clear steering");
    }
    for(int reason=0;reason<8;++reason) {
        Steering steering;Acquire(steering);
        auto in=Sample(3);
        if(reason==0) in.allowed=false;
        if(reason==1) in.headTracked=false;
        if(reason==2) ++in.generation;
        if(reason==3) ++in.handGeneration[0];
        if(reason==4) ++in.handGeneration[1];
        if(reason==5) in.seconds+=.2;
        if(reason==6) in.seconds-=.5;
        if(reason==7) in.head.x=std::numeric_limits<float>::quiet_NaN();
        auto out=steering.Update(in);
        Check(!out.engaged && Near(out.axis,0),"context/session/clock/head loss must cancel steering");
    }
    {
        Steering steering;Acquire(steering);auto in=Sample(3);
        in.tracked[0]=false;auto out=steering.Update(in);
        Check(!out.held[0],"tracking loss must release the affected hand");
        ++in.sequence;in.seconds+=1./90.;in.tracked[1]=false;out=steering.Update(in);
        Check(!out.engaged && Near(out.axis,0),"loss of both hands must clear steering");
        ++in.sequence;in.seconds+=1./90.;in.tracked[0]=in.tracked[1]=true;
        Check(!steering.Update(in).engaged,"tracking recovery with grips held must not reacquire wheel");
    }
    for(int invalid=0;invalid<8;++invalid) {
        Steering steering;auto in=Sample(0);in.squeeze[0]=in.squeeze[1]=0;steering.Update(in);
        in=Sample(1);
        if(invalid==0) for(auto& h:in.hand) h.z=.2f; // behind head
        if(invalid==1) for(auto& h:in.hand) h.z=-1.2f;
        if(invalid==2) for(auto& h:in.hand) h.y=.5f;
        if(invalid==3) for(auto& h:in.hand) h.y=2.1f;
        if(invalid==4) { in.hand[0].x=-.02f;in.hand[1].x=.02f; }
        if(invalid==5) { in.hand[0].x=-.5f;in.hand[1].x=.5f; }
        if(invalid==6) in.hand[0].z-=.4f;
        if(invalid==7) in.forward={0,1,0};
        Check(!steering.Update(in).engaged,"invalid wheel acquisition geometry must not engage");
    }
    {
        Steering steering;Acquire(steering);
        auto in=Sample(2);in.seconds+=.05;
        auto out=steering.Update(in);Check(out.engaged && Near(out.axis,0),"fresh duplicate poll retains output without integration");
        in.seconds+=.06;out=steering.Update(in);Check(!out.engaged,"duplicate sample must time out after freshness window");
    }
    {
        Steering steering;Acquire(steering);auto in=Sample(3);
        in.hand[0].x=std::numeric_limits<float>::quiet_NaN();auto out=steering.Update(in);
        Check(!out.held[0] && std::isfinite(out.axis),"invalid hand position cannot retain that grip or poison steering");
    }
    // Replay and tracking-loss regressions: validate evidence before reusing a
    // previous result, even when no new sample number has arrived.
    {
        Steering steering;Acquire(steering);auto in=Sample(1);in.seconds=10.04;
        Check(!steering.Update(in).engaged,"out-of-order sample must cancel old steering");
    }
    {
        Steering steering;Acquire(steering);auto in=Sample(2);in.seconds+=.02;in.tracked[0]=false;
        auto out=steering.Update(in);Check(!out.held[0],"same-sequence tracking loss must not retain the affected hand");
    }
    for(unsigned reason=0;reason<unsigned(SteeringReason::Count);++reason)
        Check(SteeringReasonName(SteeringReason(reason))[0]!='\0',"all steering reasons have bounded printable names");
    Check(SteeringReasonName(SteeringReason::Count)[0]=='u',"out-of-range steering diagnostic is bounded");
    std::printf("Motion steering: %u checks passed\n",checks);
    return 0;
}
