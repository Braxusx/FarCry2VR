#include "common/walking_legs.h"
#include "common/action_context.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace fc2vr::walking;
static void Check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"%s\n",what);std::exit(1); } }
static Input Sample(double time,Vec3 root={0,0,0}) {
    Input in;in.allowed=true;in.owner=1;in.generation=1;in.seconds=time;in.root=root;
    for(int i=0;i<2;++i) {
        in.ankle[i]=Add(root,{i?.1f:-.1f,0,.15f});
        in.ground[i]={true,{in.ankle[i].x,in.ankle[i].y,root.z},{0,0,1}};
    }
    return in;
}
int main() {
    Planner planner;auto initial=planner.Update(Sample(1));
    for(int i=1;i<900;++i) {
        auto o=planner.Update(Sample(1+i/90.));
        Check(o.valid && o.steps==0 && Length(Subtract(o.ankle[0],initial.ankle[0]))<1e-6f,"standing still must not march");
    }
    // Actual displacement drives both stick and room movement; there is no stick
    // input to the planner. Check world planting during each stance interval.
    for(Vec3 direction: {Vec3{0,1,0},Vec3{1,0,0},Vec3{0,-1,0}}) {
        planner.Reset();Output previous=planner.Update(Sample(1));int completed[2]={};
        for(int i=1;i<=450;++i) {
            const float t=i/90.f;auto in=Sample(1+t,Scale(direction,t*1.5f));
            auto o=planner.Update(in);Check(o.valid,"walking must stay valid");
            for(int side=0;side<2;++side) {
                if(previous.swinging!=side && o.swinging!=side)
                    Check(Length(Subtract(o.ankle[side],previous.ankle[side]))<1e-5f,"stance foot slid");
                Check(o.ankle[side].z>=.149f && o.ankle[side].z<.26f,"swing height out of bounds");
                if(previous.swinging==side && o.swinging!=side) ++completed[side];
            }
            previous=o;
        }
        Check(completed[0]>5 && completed[1]>5,"both legs must step");
    }
    // Opening/closing either on-foot map while already moving must preserve
    // the planted feet and continue the gait. A driving map still yields.
    for(const char* map: {"mapcompass","map_briefing"}) {
        planner.Reset();Output previous=planner.Update(Sample(1));
        for(int i=1;i<=600;++i) {
            const char* context=i<150 || i>=450 ? "default" : map;
            auto in=Sample(1+i/90.,{0,i/90.f,0});
            in.allowed=fc2vr::input::OnFootContext(context).locomotion;
            auto current=planner.Update(in);
            Check(current.valid && current.steps>=previous.steps,"map toggle interrupted walking");
            for(int side=0;side<2;++side) if(previous.swinging!=side && current.swinging!=side)
                Check(Length(Subtract(current.ankle[side],previous.ankle[side]))<1e-5f,"map toggle moved planted foot");
            if(i==449) Check(current.steps>5,"no steps while reading the map");
            previous=current;
        }
        auto driving=Sample(8,{0,7,0});
        driving.allowed=fc2vr::input::OnFootContext("map_driving").locomotion;
        Check(!planner.Update(driving).valid,"driving map enabled walking legs");
    }
    planner.Reset();planner.Update(Sample(1));
    auto turn=Sample(1.01);turn.forward={0,-1,0};
    auto turning=planner.Update(turn);Check(turning.swinging>=0,"physical yaw must reposition the feet");
    auto teleport=Sample(1.02,{100,100,0});auto o=planner.Update(teleport);
    Check(o.valid && o.steps==0 && Length(Subtract(o.ankle[0],teleport.ankle[0]))<1e-4f,"teleport must reset foot plants");
    teleport.owner=2;teleport.seconds+=.01;
    Check(planner.Update(teleport).steps==0,"new skeleton must reset");
    teleport.generation=2;teleport.seconds+=.01;
    Check(planner.Update(teleport).steps==0,"tracking recovery must reset");
    teleport.ground[0].valid=false;Check(!planner.Update(teleport).valid,"missing ground must yield to native pose");
    auto off=Sample(3);off.allowed=false;Check(!planner.Update(off).valid,"vehicle/script must disable gait");
    auto nan=Sample(4);nan.root.x=std::numeric_limits<float>::quiet_NaN();Check(!planner.Update(nan).valid,"reject invalid root");
    float height=0;
    Check(Height({true,{0,0,0},{0,-.2f,1}},{0,.2f,0},height) && std::fabs(height-.04f)<1e-6f,"slope height");
    Check(!Height({true,{0,0,0},{1,0,.1f}},{0,0,0},height),"wall cannot be floor");
    for(Vec3 target: {Vec3{0,0,0},Vec3{0,.2f,.1f},Vec3{1,0,-1},Vec3{0,0,.8f}}) {
        const Vec3 hip{0,0,.9f};auto pose=Solve(hip,target,{0,1,0},.45f,.46f);
        Check(pose.valid && Finite(pose.knee) && Finite(pose.ankle),"finite two-bone solution");
        Check(std::fabs(Length(Subtract(pose.knee,hip))-.45f)<1e-5f,"upper leg length changed");
        Check(std::fabs(Length(Subtract(pose.ankle,pose.knee))-.46f)<1e-5f,"lower leg length changed");
    }
    Check(!Solve({0,0,0},{0,0,0},{0,1,0},.4f,.4f).valid,"zero reach must fail");
    // Dimensions measured from the owner's 107-bone capture, not a generic rig.
    // Its original ankle is 28 cm behind a completely straight hip/knee chain;
    // the adapter centres stance under the hips before planning the steps.
    for(float hz:{60.f,90.f,120.f}) for(float speed:{.3f,1.5f,3.2f}) {
        planner.Reset();
        for(int i=0;i<600;++i) {
            auto in=Sample(1+i/double(hz),{0,i/hz*speed,0});
            for(int side=0;side<2;++side) {
                in.ankle[side].x=side?.09794f:-.09794f;
                in.ankle[side].y-=.00095f;in.ankle[side].z=.16224f;
                in.clearance[side]=.19222f;in.ground[side].point={in.ankle[side].x,in.ankle[side].y,0};
            }
            auto out=planner.Update(in);Check(out.valid,"captured rig planner");
            for(int side=0;side<2;++side) {
                Vec3 hip={side?.09794f:-.09794f,in.root.y-.00095f,.99478f};
                auto solved=Solve(hip,out.ankle[side],{0,1,0},.428092f,.450816f);
                Check(solved.valid && Length(Subtract(solved.ankle,out.ankle[side]))<.02f,"captured leg cannot reach planted foot");
            }
        }
    }
    std::puts("walking: planted stance, both legs, forward/back/sideways, turning, ownership, ground and IK passed");
}
