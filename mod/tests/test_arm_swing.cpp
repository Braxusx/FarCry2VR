#include "common/arm_swing.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace fc2vr::motion;
using namespace fc2vr::tracking;
static unsigned checks=0;
static void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
static ArmSwingInput Sample(int frame,float hz,float amplitude=.16f,float cadence=1.5f) {
    ArmSwingInput in;in.allowed=in.tracked=true;in.sequence=std::uint64_t(frame)+1;
    in.generation=3;in.handGeneration[0]=7;in.handGeneration[1]=9;
    in.seconds=10+frame/double(hz);in.head={0,1.7f,0};
    const float displacement=amplitude*std::sin(float(frame/hz*6.28318530718*cadence));
    in.hand[0]={-.25f,1.1f,-.2f-displacement};
    in.hand[1]={.25f,1.1f,-.2f+displacement};
    return in;
}
static Vec3 Rotate(Vec3 p,float yaw) {
    return {std::cos(yaw)*p.x+std::sin(yaw)*p.z,p.y,-std::sin(yaw)*p.x+std::cos(yaw)*p.z};
}
static ArmSwingInput Natural(int frame,float hz,int style) {
    auto in=Sample(frame,hz,0);const float t=frame/hz;
    const float phase=6.2831853f*(1.45f*t+.055f*std::sin(t*1.7f));
    const float lag=style>=2 ? .65f : .35f;
    const float vertical=style==0 ? .17f : (style==3 ? .025f : .105f);
    const float depth=style==0 ? .025f : (style==3 ? .15f : .115f);
    const float left=std::sin(phase),right=std::sin(phase+lag);
    in.hand[0]={-.23f+.012f*std::sin(phase+.4f),1.16f+vertical*left,-.16f-depth*left};
    in.hand[1]={.28f-.009f*std::sin(phase+.9f),1.12f-vertical*.72f*right,-.21f+depth*.78f*right};
    if(style==3) { in.hand[0].z+=.09f;in.hand[1].z+=.11f; }
    for(int h=0;h<2;++h) {
        in.hand[h].x+=.0015f*std::sin(t*117+h);
        in.hand[h].y+=.0013f*std::sin(t*89+h*2);
        in.hand[h].z+=.0017f*std::sin(t*103+h*3);
    }
    in.head.x=.025f*std::sin(t*2.3f);in.head.y+=.018f*std::sin(t*4.1f);
    in.head.z=.02f*std::sin(t*2.9f);
    const float bodyYaw=.3f*t+.13f*std::sin(t*1.3f);
    in.head=Rotate(in.head,bodyYaw);for(auto& h:in.hand) h=Rotate(h,bodyYaw);
    in.forward=Rotate(in.forward,bodyYaw+1.2f*std::sin(t*.9f));
    const Vec3 shift{t*.1f,.01f*std::sin(t),t*.07f};
    in.head=Add(in.head,shift);for(auto& h:in.hand) h=Add(h,shift);
    return in;
}
static ArmSwingInput Vigorous(int frame,float hz,float amplitude,float cadence,bool raised) {
    auto in=Sample(frame,hz,0);const float t=frame/hz,phase=t*6.2831853f*cadence;
    const float left=std::sin(phase),right=std::sin(phase+.25f);
    in.hand[0].z-=amplitude*left;in.hand[1].z+=amplitude*.85f*right;
    const float height=raised?1.55f:1.1f,vertical=raised?.45f:.15f;
    in.hand[0].y=height+amplitude*vertical*left;
    in.hand[1].y=height-.03f-amplitude*vertical*.85f*right;
    for(int h=0;h<2;++h) {
        in.hand[h].x+=.0015f*std::sin(t*117+h);
        in.hand[h].z+=.0017f*std::sin(t*103+h*3);
    }
    in.head.x=.02f*std::sin(t*2.3f);in.head.y+=.015f*std::sin(t*4.1f);
    const float bodyYaw=.2f*t;
    in.head=Rotate(in.head,bodyYaw);for(auto& h:in.hand) h=Rotate(h,bodyYaw);
    in.forward=Rotate(in.forward,bodyYaw+.8f*std::sin(t));
    return in;
}
int main() {
    for(float hz:{60.f,90.f,120.f}) for(float amplitude:{.3f,.4f,.5f})
        for(float cadence:{1.5f,2.f,2.5f}) for(bool raised:{false,true}) {
            ArmSwingSprint detector;float fastest=0;
            for(int i=0;i<int(hz*6);++i) {
                const bool active=detector.Update(Vigorous(i,hz,amplitude,cadence,raised));
                const auto report=detector.Diagnostics();fastest=std::max(fastest,std::max(report.rawLeftSpeed,report.rawRightSpeed));
                if(i>int(hz*2)) Check(active,"vigorous raised or lowered arm pumping must sustain established sprint");
            }
            if(amplitude==.5f && cadence==2.5f) Check(fastest>7.f,"vigorous regression must cross the former 5 m/s ceiling");
        }
    for(float hz:{60.f,90.f,120.f}) for(int hand=0;hand<3;++hand) for(float jump:{-.3f,.3f}) {
        ArmSwingSprint detector;for(int i=0;i<=200;++i) detector.Update(Sample(i,hz));
        Check(detector.Diagnostics().active,"teleport rejection precondition");
        auto in=Sample(200,hz);++in.sequence;in.seconds+=1./hz;
        if(hand!=1) in.hand[0].x+=jump;
        if(hand!=0) in.hand[1].x+=jump;
        Check(!detector.Update(in),"single-frame tracking teleports cancel vigorous-compatible sprint immediately");
        const auto report=detector.Diagnostics();
        Check(report.reason==ArmSwingReason::SpeedJump,"teleports have a distinct diagnostic reason");
        Check((hand==1 || report.rawLeftSpeed>12) && (hand==0 || report.rawRightSpeed>12),
            "raw rejection speeds survive cancellation for runtime diagnosis");
    }
    for(float hz:{60.f,90.f,120.f}) for(int style=0;style<4;++style) {
        ArmSwingSprint detector;
        for(int i=0;i<int(hz*8);++i) {
            const bool active=detector.Update(Natural(i,hz,style));
            if(i>int(hz*2)) Check(active,"bent-elbow asymmetric noisy pumping must sustain sprint");
        }
    }
    for(float hz:{60.f,90.f,120.f}) for(int mode=0;mode<2;++mode) {
        ArmSwingSprint detector;
        for(int i=0;i<int(hz*6);++i) {
            auto in=Sample(i,hz,0);const float t=i/hz;
            // Eighty millisecond pauses, independent of the XR sample clock.
            const double block=std::floor(double(t)/.7),cycle=double(t)-block*.7;
            const float movingTime=float(double(t)-block*.08-std::max(0.,cycle-.62));
            const float d=.15f*std::sin(movingTime*6.2831853f*1.5f);
            if(mode==0) { in.hand[0].z-=d;in.hand[1].z+=d; }
            else { in.hand[0].y+=d;in.hand[1].y-=d; }
            const bool active=detector.Update(in);
            if(t>2) {
                if(!active) { const auto report=detector.Diagnostics();std::fprintf(stderr,"pause hz=%.0f mode=%d t=%.3f reason=%s stroke=%.3f travel=%.3f/%.3f span=%.3f\n",hz,mode,t,ArmSwingReasonName(report.reason),report.strokeSeconds,report.leftTravel,report.rightTravel,report.spanVariation); }
                Check(active,"brief natural pauses retain established alternating cadence");
            }
        }
    }
    for(float hz:{60.f,90.f,120.f}) for(int mode=0;mode<3;++mode) {
        ArmSwingSprint detector;
        for(int i=0;i<int(hz*6);++i) {
            auto in=Sample(i,hz,0);const float t=i/hz;
            if(mode<2) { // rigid-body turns, including hands centred on the torso
                const float yaw=.8f*std::sin(t*5);
                for(auto& hand:in.hand) { if(mode==1) hand.z=0;hand=Rotate(hand,yaw); }
                in.forward=Rotate(in.forward,yaw);
            } else { // one actual arm pumps while the head translates normally
                in.hand[0].y+=.12f*std::sin(t*9);in.hand[0].z-=.16f*std::sin(t*9);
            }
            in.head.x=.025f*std::sin(t*4);in.head.y+=.018f*std::sin(t*8);
            in.head.z=.02f*std::sin(t*7);
            for(int h=0;h<2;++h) {
                in.hand[h].x+=.0015f*std::sin(t*117+h);
                in.hand[h].y+=.0013f*std::sin(t*89+h*2);
                in.hand[h].z+=.0017f*std::sin(t*103+h*3);
            }
            Check(!detector.Update(in),"tracking noise/head translation cannot make rigid turns or one arm count as running");
        }
    }
    for(float hz:{60.f,90.f,120.f}) {
        ArmSwingSprint detector;const int end=int(hz*3);const auto stopped=Sample(end,hz);
        for(int i=0;i<=end;++i) detector.Update(Sample(i,hz));
        Check(detector.Diagnostics().active,"one-arm stopping precondition");
        for(int i=end+1;i<=end+int(hz);++i) {
            auto in=Sample(i,hz);in.hand[1]=stopped.hand[1];
            const bool active=detector.Update(in);
            if(i>end+int(hz*.35f)) Check(!active,"stopping either arm promptly cancels sprint");
        }
    }
    // Looking independently of the torso must not redefine a running stroke.
    // Includes looking sideways before acquisition, looking during a run and
    // a simultaneous physical body turn; all pose timestamps remain fresh.
    for(float hz: {60.f,90.f,120.f}) for(int mode=0;mode<5;++mode) {
        ArmSwingSprint detector;
        for(int i=0;i<int(hz*5);++i) {
            auto in=Sample(i,hz);
            const float t=i/hz;
            const float bodyYaw=mode==2 ? t*.25f : 0.f;
            const float look=(mode==0 || mode==3) ? 1.5707963f : 1.5707963f*std::sin(t*1.4f);
            if(mode>=3) for(auto& hand:in.hand) hand.z+=.2f; // pumping around torso, no forward midpoint
            for(auto& hand:in.hand) hand=Rotate(hand,bodyYaw);
            in.forward=Rotate(in.forward,bodyYaw+look);
            const bool active=detector.Update(in);
            if(t>1.5f) Check(active,"independent head yaw must not erase running strokes");
        }
    }
    // The input adapter used GetTickCount64 for 90 Hz XR poses. Model its
    // 15.625 ms system ticks (reported as integer milliseconds), including a
    // phase offset and nonuniform XR frame intervals. New pose sequences can
    // share one host timestamp. The XR display timestamp remains independent.
    for(bool xrClock: {false,true}) {
        ArmSwingSprint detector;
        std::int64_t elapsedNs=7000000;
        const std::int64_t jitter[]={0,350000,-350000,150000,-150000};
        double lastClock=-1;unsigned repeated=0;
        for(int i=0;i<360;++i) {
            if(i) elapsedNs+=11111111+jitter[i%5];
            const double t=double(elapsedNs-7000000)*1e-9;
            auto in=Sample(i,90);
            const float displacement=.16f*std::sin(float(t*6.28318530718*1.5));
            in.hand[0].z=-.2f-displacement;in.hand[1].z=-.2f+displacement;
            const auto hostMs=((elapsedNs/15625000)*15625000)/1000000;
            in.seconds=xrClock ? 9000+double(elapsedNs)*1e-9 : 2000+double(hostMs)*.001;
            if(in.seconds==lastClock) ++repeated;
            lastClock=in.seconds;
            const bool active=detector.Update(in);
            if(t>1.5) Check(active,"coarse adapter clock must not repeatedly erase natural sprint strokes");
        }
        Check(xrClock ? repeated==0 : repeated>50,"clock regression must actually contain coarse repeated timestamps");
    }
    for(float hz: {60.f,90.f,120.f}) for(float cadence: {1.f,1.5f,2.f}) {
        ArmSwingSprint detector;bool ever=false;int continuing=0;
        for(int i=0;i<int(hz*4);++i) {
            const bool active=detector.Update(Sample(i,hz,.16f,cadence));
            ever|=active;
            if(i>int(hz*1.5)) { Check(active,"continuous natural pumping must retain sprint");++continuing; }
        }
        Check(ever && continuing>0,"alternating arm pumping must start sprint");
        auto stationary=Sample(int(hz*4),hz,.16f,cadence);
        for(int i=1;i<=int(hz*.4);++i) {
            stationary.seconds=14+i/double(hz);++stationary.sequence;
            bool active=detector.Update(stationary);
            if(i>int(hz*.3)) Check(!active,"sprint must end shortly after arms stop");
        }
    }
    for(int revocation=0;revocation<3;++revocation) {
        ArmSwingSprint detector;
        for(int i=0;i<=200;++i) detector.Update(Sample(i,90));
        auto in=Sample(200,90);++in.sequence;
        Check(detector.Update(in),"new sequence with tied time must retain existing sprint");
        if(revocation==0) in.allowed=false;
        if(revocation==1) in.tracked=false;
        if(revocation==2) ++in.handGeneration[0];
        ++in.sequence;
        Check(!detector.Update(in),"same-time context/tracking/generation loss must still cancel sprint");
    }
    for(int mode=0;mode<10;++mode) {
        ArmSwingSprint detector;
        for(int i=0;i<600;++i) {
            auto in=Sample(i,90);
            const float t=i/90.f;
            if(mode==0) in.hand[1]={.25f,1.1f,-.2f}; // one-handed wave/reload
            if(mode==1) in.hand[1].z=in.hand[0].z; // same-direction reaches
            if(mode==2) { // whole body translation while holding hands still
                in=Sample(i,90,0);Vec3 shift{t*.4f,.02f*std::sin(t),t*.7f};
                in.head=Add(in.head,shift);for(auto& h:in.hand) h=Add(h,shift);
            }
            if(mode==3) in=Sample(i,90,.012f); // controller tremor/noise
            if(mode==4) { // physical yaw with head and hands together
                in=Sample(i,90,0);float yaw=.6f*std::sin(t*3);
                in.forward=Rotate(in.forward,yaw);for(auto& h:in.hand) h=Rotate(h,yaw);
            }
            if(mode==5) { // looking left/right with stationary hands
                in=Sample(i,90,0);in.forward=Rotate(in.forward,.6f*std::sin(t*3));
            }
            if(mode==6) { // raising both hands together is not alternating pumping
                in=Sample(i,90,0);float y=.15f*std::sin(t*8);in.hand[0].y+=y;in.hand[1].y+=y;
            }
            if(mode==7) { // common lateral reaching rotates the midpoint axis
                in=Sample(i,90,0);const float x=.2f*std::sin(t*8);
                in.hand[0].x+=x;in.hand[1].x+=x;
            }
            if(mode==8) { // head translating beside two stationary controllers
                in=Sample(i,90,0);in.head.x=.2f*std::sin(t*8);
            }
            if(mode==9) { // hands at torso sides, rotating with body, no strokes
                in=Sample(i,90,0);const float yaw=.8f*std::sin(t*3);
                for(auto& hand:in.hand) { hand.z=0;hand=Rotate(hand,yaw); }
                in.forward=Rotate(in.forward,yaw);
            }
            Check(!detector.Update(in),"non-running gestures must not sprint");
        }
    }
    for(float hz:{60.f,90.f,120.f}) {
        ArmSwingSprint detector;
        for(int i=0;i<int(hz*5);++i) {
            auto in=Sample(i,hz);const float t=i/hz,phase=t*6.2831853f*1.5f;
            in.hand[0].x=-.22f+.015f*std::sin(phase);
            in.hand[1].x=.28f-.012f*std::sin(phase+.25f);
            in.hand[1].z=-.18f+.12f*std::sin(phase+.25f);
            in.forward=Rotate(in.forward,1.5f*std::sin(t));
            const bool active=detector.Update(in);
            if(t>1.5f) Check(active,"unequal natural strokes with independent head look retain sprint");
        }
    }
    // Pumping remains recognisable while walking and making a physical turn.
    {
        ArmSwingSprint detector;
        for(int i=0;i<600;++i) {
            auto in=Sample(i,90);float yaw=i/90.f*.3f;Vec3 shift{yaw,0,yaw*.5f};
            in.forward=Rotate(in.forward,yaw);in.head=Add(Rotate(in.head,yaw),shift);
            for(auto& h:in.hand) h=Add(Rotate(h,yaw),shift);
            bool active=detector.Update(in);if(i>135) Check(active,"motion in the room must not cancel arm pumping");
        }
    }
    {
        ArmSwingSprint detector;
        for(int i=0;i<200;++i) detector.Update(Sample(i,90));
        Check(detector.Update(Sample(200,90)),"small-motion cancellation precondition");
        for(int i=201;i<400;++i) {
            const bool active=detector.Update(Sample(i,90,.012f,4.f));
            if(i>250) Check(!active,"tiny fast tremor must not sustain an existing sprint");
        }
    }
    for(int failure=0;failure<9;++failure) {
        ArmSwingSprint detector;
        for(int i=0;i<200;++i) detector.Update(Sample(i,90));
        auto in=Sample(200,90);
        Check(detector.Update(in),"precondition active sprint");
        ++in.sequence;in.seconds+=1./90.;
        if(failure==0) in.allowed=false;
        if(failure==1) in.tracked=false;
        if(failure==2) ++in.generation;
        if(failure==3) ++in.handGeneration[1];
        if(failure==4) in.hand[0].z=std::numeric_limits<float>::quiet_NaN();
        if(failure==5) in.seconds+=.2;
        if(failure==6) in.seconds-=.5;
        if(failure==7) in.sequence-=10;
        if(failure==8) in.hand[0].z+=.7f;
        Check(!detector.Update(in),"invalid state must cancel sprint immediately");
        const auto reason=failure==0?ArmSwingReason::NotAllowed:failure==1?ArmSwingReason::Tracking:
            failure==4?ArmSwingReason::InvalidSample:failure==5?ArmSwingReason::Stale:
            failure==6?ArmSwingReason::Clock:failure==7?ArmSwingReason::Replay:
            failure==8?ArmSwingReason::SpeedJump:ArmSwingReason::Generation;
        Check(detector.Diagnostics().reason==reason,"diagnostics identify why a gesture was cancelled");
    }
    {
        ArmSwingSprint detector;auto in=Sample(0,90);
        for(int i=0;i<1000;++i) {
            in.seconds+=.001;in.hand[0].z=(i&1)?-.5f:0;in.hand[1].z=-in.hand[0].z;
            Check(!detector.Update(in),"repeated sample sequence must not create gestures");
        }
        detector.Reset();for(int i=0;i<200;++i) detector.Update(Sample(i,90));
        in=Sample(200,90);Check(detector.Update(in),"duplicate timeout precondition");
        in.seconds+=.05;Check(detector.Update(in),"short duplicate poll holds existing output");
        in.seconds+=.06;Check(!detector.Update(in),"stale sample must cancel within tracking freshness window");
    }
    for(unsigned reason=0;reason<unsigned(ArmSwingReason::Count);++reason)
        Check(ArmSwingReasonName(ArmSwingReason(reason))[0]!='\0',"every diagnostic reason has a printable name");
    Check(ArmSwingReasonName(ArmSwingReason::Count)[0]=='u',"invalid diagnostic reason remains bounded");
    std::printf("Arm-swing sprint: %u checks passed\n",checks);
    return 0;
}
