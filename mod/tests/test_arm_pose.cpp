#include "common/arm_pose.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
using namespace fc2vr;
namespace {
int checks=0, failures=0;
void Check(bool ok,const char* why) { ++checks; if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
bool Near(float a,float b,float eps=2e-4f) { return std::fabs(a-b)<eps; }
authority::Stamp Sample(std::uint64_t i=1) {
    authority::Stamp s; s.sequence=i; s.receivedMs=1000+i*10; s.generation=2;
    s.handGeneration[0]=3; s.handGeneration[1]=4; s.focused=s.synced=true; return s;
}
arm::RigKey Key() { arm::RigKey k; k.pawn=1; k.skeleton=2; k.palette=3; k.weapon=4; k.count=120; k.upper=10; k.fore=11; k.hand=12; return k; }
void Twist() {
    for (float weight : {0.1f,0.5f,1.0f}) {
        for (int direction : {-1,1}) {
            arm::TwistHistory h; float last=0;
            for (int i=0; i<401; ++i) {
                const float angle=direction*(160+i*0.1f)*arm::Pi/180;
                float value=0;
                Check(h.Resolve(arm::Wrap(angle),Key(),Sample(i+1),1,true,value),"continuous sample accepted");
                Check(Near(value,angle),"unwrap crosses +/-pi without flipping");
                if (i) Check(std::fabs(value*weight-last)<0.002f,"fractional twist is continuous");
                last=value*weight;
                const float frozen=value;
                Check(h.Resolve(arm::Wrap(angle+0.5f),Key(),Sample(i+1),1,true,value) && value==frozen,"repeat sample cannot advance twist");
            }
        }
    }
    const float oldJump=std::fabs(arm::Wrap(180.1f*arm::Pi/180)*0.5f-arm::Wrap(179.9f*arm::Pi/180)*0.5f);
    Check(oldJump>3.13f,"negative control: old fractional wrap flips about pi");
    arm::TwistHistory h; float v=0;
    for (int i=0;i<2000;++i) {
        Check(h.Resolve(arm::Wrap(i*0.1f),Key(),Sample(i+1),0,true,v),"multiple-turn stream valid");
        Check(std::fabs(v)<=arm::Tau,"anatomical winding bounded");
    }
    const float saturated=v;
    Check(h.Resolve(arm::Wrap(199.8f),Key(),Sample(2001),0,true,v) && v<saturated,"reverse responds immediately after saturation");
}
void Reset() {
    for(int reason=0;reason<14;++reason) {
        arm::TwistHistory h; auto k=Key(); auto s=Sample(); float v=0;
        h.Resolve(3.13f,k,s,1,true,v); s=Sample(2); h.Resolve(-3.13f,k,s,1,true,v);
        Check(v>arm::Pi,"precondition: branch unwrapped");
        s=Sample(3);
        if(reason==0) ++k.pawn;
        if(reason==1) ++k.skeleton;
        if(reason==2) ++k.palette;
        if(reason==3) ++k.weapon;
        if(reason==4) ++k.count;
        if(reason==5) ++k.upper;
        if(reason==6) ++k.fore;
        if(reason==7) ++k.hand;
        if(reason==8) ++k.mode;
        if(reason==9) ++s.generation;
        if(reason==10) ++s.handGeneration[1];
        if(reason==11) s.receivedMs+=200;
        if(reason==12) h.Clear();
        if(reason==13) { Check(!h.Resolve(0,k,s,1,false,v),"loss revokes history"); s=Sample(4); }
        Check(h.Resolve(-3.12f,k,s,1,true,v) && Near(v,-3.12f),"context/rig/loss/gap resets winding");
    }
    arm::TwistHistory h; float v=0; auto s=Sample();
    Check(!h.Resolve(std::numeric_limits<float>::quiet_NaN(),Key(),s,1,true,v),"NaN twist rejected");
    s.sequence=0; Check(!h.Resolve(0,Key(),s,1,true,v),"unpublished twist rejected");
    h.Resolve(3.13f,Key(),Sample(5),1,true,v);
    h.Bind(Key(),Sample(4),1);
    Check(!h.Resolve(-3.13f,Key(),Sample(4),1,true,v),"out-of-order source rejected even after binding");
    Check(h.Resolve(-3.13f,Key(),Sample(6),1,true,v) && v>arm::Pi,"old source cannot erase newer winding");
}
void Projection() {
    float angle=0;
    Check(arm::ProjectedAngle({1,0,0},{0,1,0},{0,0,1},angle) && Near(angle,arm::Pi/2),"signed quarter turn");
    Check(arm::ProjectedAngle({1,0,2},{0,-1,5},{0,0,3},angle) && Near(angle,-arm::Pi/2),"axial component removed");
    Check(!arm::ProjectedAngle({0,0,1},{1,0,0},{0,0,1},angle),"axis-aligned projection unavailable");
    Check(!arm::ProjectedAngle({1,0,0},{0,1,0},{0,0,0},angle),"zero axis unavailable");
    Check(!arm::ProjectedAngle({1,0,0},{0,1,0},{0,0,std::numeric_limits<float>::infinity()},angle),"nonfinite unavailable");
    for(int i=-360;i<=360;++i) {
        const float a=i*arm::Pi/180;
        Check(arm::ProjectedAngle({1,0,0},{std::cos(a),std::sin(a),0},{0,0,1},angle),"sweep projected");
        Check(std::fabs(arm::Wrap(angle-a))<1e-5f,"signed projected sweep");
    }
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!std::strcmp(argv[1],"twist")) Twist();
    else if(!std::strcmp(argv[1],"reset")) Reset();
    else if(!std::strcmp(argv[1],"projection")) Projection();
    else return 2;
    std::printf("%s: %d checks, %d failures\n",argv[1],checks,failures); return failures?1:0;
}
