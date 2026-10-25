#include "common/arm_rig.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <string>
#include <initializer_list>
#include <iostream>
using namespace fc2vr;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
bool Near(tracking::Vec3 a,tracking::Vec3 b,float eps=3e-4f) { const auto d=tracking::Subtract(a,b); return tracking::DotProduct(d,d)<eps*eps; }
float Length(tracking::Vec3 p) { return std::sqrt(tracking::DotProduct(p,p)); }
authority::Stamp Sample(std::uint64_t n=1) { authority::Stamp s; s.sequence=n; s.receivedMs=1000+10*n; s.generation=2; s.handGeneration[0]=3; s.handGeneration[1]=4; s.focused=s.synced=true; return s; }
arm::RigKey Key() { arm::RigKey k; k.pawn=1; k.skeleton=2; k.palette=3; k.weapon=4; k.count=107; k.upper=54; k.fore=55; k.hand=56; k.mode=0x100; return k; }
struct Fixture {
    std::vector<rig::Identity> ids;
    int Add(const std::string& name,int parent) { ids.push_back({rig::NameId(name.c_str()),parent}); return static_cast<int>(ids.size())-1; }
    Fixture() {
        const int root=Add("Root",-1),pelvis=Add("Pelvis",root),spine=Add("Spine2",pelvis); Add("Camera",spine);
        for(const char* side:{"L ","R "}) {
            const auto name=[&](const char* suffix) { return std::string(side)+suffix; };
            int clav=Add(name("Clavicle"),spine), upper=Add(name("UpperArm"),clav),fore=Add(name("Forearm"),upper),hand=Add(name("Hand"),fore);
            Add(name("U UpperArm twist"),upper); Add(name("D UpperArm twist"),upper);
            for(const char* s:{"D Forearm twist U","M Forearm twist","U Forearm twist D","B Forearm twist"}) Add(name(s),fore);
            for(const char* digit:{"Thumb","Index","Middle","Ring","Pinky"}) {
                int p=hand; for(const char* n:{"01","02","03"}) p=Add(name((std::string(digit)+n).c_str()),p);
            }
            Add(name("Elbow"),pelvis);
        }
        Add("R Knee",pelvis); Add("L Knee",pelvis);
    }
};
void Mapping() {
    Check(rig::NameId("123456789")==0xcbf43926u,"CRC known control");
    Check(rig::NameId("L Hand")!=rig::NameId("l Hand"),"case sensitive identity");
    Fixture f;
    for(int side=0;side<2;++side) {
        rig::Roles roles;
        Check(rig::Resolve(f.ids.data(),static_cast<int>(f.ids.size()),side,roles),"named fixture resolved");
        auto owned=roles.Indices(); Check(owned.size()==25,"exact 25-record write set");
        for(int index:owned) Check(f.ids[index].name!=rig::NameId("Camera") && f.ids[index].name!=rig::NameId("R Knee") && f.ids[index].name!=rig::NameId("L Knee"),"no camera/torso/knee ownership");
        auto reversed=f.ids; const int count=static_cast<int>(reversed.size());
        for(int i=0;i<count;++i) { reversed[count-1-i]=f.ids[i]; reversed[count-1-i].parent=f.ids[i].parent<0?-1:count-1-f.ids[i].parent; }
        rig::Roles renumbered; Check(rig::Resolve(reversed.data(),count,side,renumbered),"renumbered non-topological array resolved");
        Check(renumbered.hand[0]==count-1-roles.hand[0] && renumbered.cap==count-1-roles.cap,"no fixed index subtraction");
        auto invalidAncestor=reversed; invalidAncestor[count-1].parent=999;
        Check(!rig::Resolve(invalidAncestor.data(),count,side,renumbered),"all parent ranges checked before traversing a future ancestor");
        for(int reason=0;reason<6;++reason) {
            auto invalid=f.ids;
            if(reason==0) invalid[roles.fore[2]].name=0;
            if(reason==1) invalid.push_back({invalid[roles.fore[2]].name,invalid[roles.fore[2]].parent});
            if(reason==2) invalid[roles.cap].parent=roles.fore[0];
            if(reason==3) invalid[roles.hand[5]].parent=roles.hand[0];
            if(reason==4) invalid[0].parent=roles.hand[0];
            if(reason==5) invalid[roles.upper[1]].parent=999;
            Check(!rig::Resolve(invalid.data(),static_cast<int>(invalid.size()),side,renumbered),"missing/duplicate/wrong parent/cycle rejected");
        }
    }
}
rig::Arm Rest() {
    rig::Arm a;
    a.upper[0].p={0,0,0}; a.upper[1].p={0,0,0}; a.upper[2].p={0.1f,0,0};
    a.fore[0].p={0.3f,0,0};
    const float weights[]={2.f/3,0.5f,1.f/3,1.f/6};
    for(int i=0;i<4;++i) a.fore[i+1].p={0.3f,0.3f*weights[i],0};
    for(int i=0;i<16;++i) a.hand[i].p={0.3f+(i?0.001f*i:0),0.3f,0};
    a.cap.p={0.31f,0.02f,0.03f}; return a;
}
void Targets(const rig::Arm& rest,weapon::Pose (&hand)[16],float angle,tracking::Vec3 axis={0,1,0},tracking::Vec3 shift={}) {
    for(int i=0;i<16;++i) { hand[i]=rig::RotatePose(rest.hand[i],rest.hand[0].p,axis,angle); hand[i].p=tracking::Add(hand[i].p,shift); }
}
bool Deform(const rig::Arm& rest,const weapon::Pose (&hand)[16],const authority::Stamp& s,rig::State& state,rig::Arm& out,rig::Hinge& joints,arm::RigKey key=Key()) {
    return rig::Deform(rest,hand,{0,0,0},{1,-1,0},{1,1,0},true,key,s,0,arm::Pi/2,state,out,joints);
}
void Joints() {
    for(int i=0;i<720;++i) {
        const float t=i*arm::Pi/180;
        const tracking::Vec3 target{std::cos(t)*0.5f,std::sin(t)*0.4f,std::sin(t*0.7f)*0.3f};
        for(bool keep:{false,true}) {
            rig::Hinge h;
            Check(rig::SolveHinge({0,0,0},target,0.3f,0.27f,{0,-1,-1},{},{1,0,-1},{1,0,0},keep,h),"3D hinge solves");
            Check(std::fabs(Length(tracking::Subtract(h.elbow,h.shoulder))-0.3f)<1e-4f,"upper length exact");
            Check(std::fabs(Length(tracking::Subtract(h.wrist,h.elbow))-0.27f)<1e-4f,"fore length exact");
            Check(keep?Near(h.wrist,target):Near(h.shoulder,{0,0,0}),"reach policy preserves selected endpoint");
            const auto direction=tracking::Subtract(h.wrist,h.shoulder);
            Check(std::fabs(tracking::DotProduct(direction,h.pole))<1e-4f,"elbow remains on feasible circle");
        }
    }
    for(float b:{0.3f,0.03f,0.8f}) {
        rig::Hinge h;
        Check(rig::SolveHinge({},{},0.3f,b,{1,0,0},{},{0,1,0},{1,0,0},true,h),"coincident wrist uses deterministic direction");
        Check(std::fabs(Length(tracking::Subtract(h.elbow,h.shoulder))-0.3f)<1e-4f,"folded upper length");
        Check(std::fabs(Length(tracking::Subtract(h.wrist,h.elbow))-b)<1e-4f,"folded fore length");
    }
    rig::Hinge distant;
    Check(rig::SolveHinge({10000,5000,100},{10000.4f,5000.1f,100.1f},0.3f,0.27f,{0,-1,-1},{},{1,0,0},{1,0,0},true,distant),"translated game-world solve");
    Check(std::fabs(Length(tracking::Subtract(distant.elbow,distant.shoulder))-0.3f)<0.002f
        && std::fabs(Length(tracking::Subtract(distant.wrist,distant.elbow))-0.27f)<0.002f,"game-world lengths within float precision");
}
void Singular() {
    tracking::Vec3 previous{}; rig::Hinge h;
    for(int i=-100;i<=100;++i) {
        const tracking::Vec3 preferred{1,0.001f*i,0};
        Check(rig::SolveHinge({},{0.4f,0,0},0.3f,0.3f,preferred,previous,{0,0,1},{1,0,0},true,h),"pole singularity safe");
        if(i>-100) Check(tracking::DotProduct(h.pole,previous)>0.9f,"no elbow hemisphere flip through pole zero");
        previous=h.pole;
        Check(std::fabs(Length(tracking::Subtract(h.elbow,h.shoulder))-0.3f)<1e-4f,"singular pole retains elbow radius");
    }
    weapon::Pose input; input.p={0,0,0}; weapon::Pose output;
    Check(rig::Swing(input,{0,0,0},{1,0,0},{2,3,4},{1,3,4},{0,1,0},output),"opposite segment rotates");
    Check(Near(output.Point({1,0,0}),{1,3,4}),"180-degree endpoint closes (old error was 2 units)");
    Check(weapon::Rigid(output),"opposite segment finite rigid basis");
    rig::Hinge untouched; untouched.elbow={7,8,9};
    Check(!rig::SolveHinge({},{},0,0.3f,{},{},{},{},true,untouched) && Near(untouched.elbow,{7,8,9}),"invalid lengths leave output untouched");
}
void Wrist() {
    const auto rest=Rest();
    for(int direction:{-1,1}) {
        rig::State state;
        for(int i=0;i<=400;++i) {
            const float angle=direction*(i*0.5f)*arm::Pi/180;
            weapon::Pose hands[16]; Targets(rest,hands,angle);
            rig::Arm out; rig::Hinge h;
            Check(Deform(rest,hands,Sample(i+1),state,out,h),"wrist roll stage accepted");
            Check(Near(out.hand[0].r,hands[0].r) && Near(out.hand[0].p,hands[0].p),"palm/weapon untouched");
            Check(Near(out.fore[0].r,rest.fore[0].r),"elbow joint not given wrist roll");
            const float applied=std::fmax(-arm::Pi/2,std::fmin(arm::Pi/2,angle));
            for(int j=1;j<5;++j) {
                const float weight=rest.fore[j].p.y/0.3f;
                const auto expected=rig::Rotate(rest.fore[j].r,{0,1,0},applied*weight);
                Check(Near(out.fore[j].r,expected),"authored longitudinal helper share, not array ordinal");
                Check(Near(out.fore[j].p,rest.fore[j].p),"axial helpers stay on segment");
            }
            Check(Near(out.cap.r,rest.cap.r) && Near(out.cap.p,rest.cap.p),"elbow cap does not follow wrist roll");
        }
    }
    weapon::Pose hands[16]; Targets(rest,hands,0.7f,{1,0,0}); rig::State state; rig::Arm out; rig::Hinge h;
    Check(Deform(rest,hands,Sample(),state,out,h) && Near(out.fore[1].r,rest.fore[1].r),"pure wrist flex is not distributed as pronation");
    float value=0;
    Targets(rest,hands,arm::Pi,{1,0,0});
    Check(!rig::WristResidual(rest.hand[0],hands[0],{0,1,0},value),"undefined perpendicular 180 swing rejected");
}
void Cap() {
    const auto rest=Rest(); weapon::Pose hands[16]; Targets(rest,hands,0,{0,1,0},{0.2f,-0.1f,0.2f});
    rig::State state; rig::Arm out; rig::Hinge h;
    Check(Deform(rest,hands,Sample(),state,out,h),"translated elbow stage accepted");
    const float oldOffset=Length(tracking::Subtract(rest.cap.p,rest.fore[0].p));
    Check(std::fabs(Length(tracking::Subtract(out.cap.p,h.elbow))-oldOffset)<1e-4f,"authored cap offset retained at solved elbow");
    Check(weapon::Rigid(out.cap),"cap hinge-frame basis rigid");
    const auto cap=out.cap;
    Targets(rest,hands,1.0f,{0,1,0},{0.2f,-0.1f,0.2f});
    Check(Deform(rest,hands,Sample(2),state,out,h) && Near(cap.p,out.cap.p) && Near(cap.r,out.cap.r),"wrist movement alone cannot steer elbow corrective");
    weapon::Pose positive,negative;
    Check(rig::HingeFrame({},{1,0,0},{2,0.0001f,0},{0,1,0},positive)
        && rig::HingeFrame({},{1,0,0},{2,-0.0001f,0},{0,1,0},negative),"nearly straight corrective frames available");
    Check(Near(positive.b,negative.b),"authored hemisphere prevents elbow-cap inversion across straight rest pose");
}
void Mesh() {
    const auto rest=Rest(); weapon::Pose hands[16]; Targets(rest,hands,arm::Pi/2);
    rig::State state; rig::Arm out; rig::Hinge h;
    Check(Deform(rest,hands,Sample(),state,out,h),"synthetic mesh setup");
    for(int i=0;i<360;++i) {
        const float a=i*arm::Pi/180;
        const tracking::Vec3 vertex{0.3f+0.02f*std::cos(a),0.29f,0.02f*std::sin(a)};
        const auto palm=out.hand[0].Point(rest.hand[0].LocalPoint(vertex));
        const auto gradient=out.fore[1].Point(rest.fore[1].LocalPoint(vertex));
        const auto old=tracking::Scale(tracking::Add(vertex,palm),0.5f);
        const auto smooth=tracking::Scale(tracking::Add(gradient,palm),0.5f);
        const auto radius=[](tracking::Vec3 v) { return std::sqrt((v.x-0.3f)*(v.x-0.3f)+v.z*v.z); };
        Check(radius(smooth)>radius(old)+0.004f,"synthetic weighted wrist ring pinches less with axial gradient");
        Check(radius(smooth)<=0.02001f,"synthetic LBS does not gain volume");
    }
}
void Staging() {
    auto rest=Rest(); weapon::Pose hand[16]; Targets(rest,hand,0.5f);
    for(int cause=0;cause<7;++cause) {
        auto bad=rest; rig::State state; state.source.sequence=7; rig::Arm out; out.cap.p={7,8,9}; rig::Hinge h; h.elbow={4,5,6};
        if(cause==0) bad.cap.r.x=std::numeric_limits<float>::quiet_NaN();
        if(cause==1) bad.fore[4].r={2,0,0};
        if(cause==2) bad.upper[0].p=bad.fore[0].p;
        if(cause==3) bad.fore[4].p={99,99,99};
        auto s=Sample(); if(cause==4) s.synced=false;
        if(cause==5) hand[15].b.x=std::numeric_limits<float>::infinity();
        const float limit=cause==6?4:arm::Pi/2;
        Check(!rig::Deform(bad,hand,{},{1,-1,0},{1,1,0},true,Key(),s,0,limit,state,out,h),"bad late member/config/input rejects full stage");
        Check(Near(out.cap.p,{7,8,9}) && Near(h.elbow,{4,5,6}) && state.source.sequence==7,"failed stage leaves output and history unchanged");
        Targets(rest,hand,0.5f);
    }
    Targets(rest,hand,0,{0,1,0},{3,0,0}); rig::State state; rig::Arm out; rig::Hinge h;
    Check(rig::Deform(rest,hand,{},{1,-1,0},{1,1,0},false,Key(),Sample(),0,arm::Pi/2,state,out,h),"free hand outside reach clamps as one staged set");
    Check(Near(h.shoulder,{}) && Length(h.wrist)<0.6f && Near(tracking::Subtract(out.hand[15].p,out.hand[0].p),tracking::Subtract(hand[15].p,hand[0].p)),"free-hand clamp preserves finger offsets and shoulder");
}
void History() {
    const auto rest=Rest(); weapon::Pose hand[16]; rig::State state; rig::Arm out; rig::Hinge h;
    Targets(rest,hand,179*arm::Pi/180); Check(Deform(rest,hand,Sample(2),state,out,h),"history start");
    Targets(rest,hand,-179*arm::Pi/180); Check(Deform(rest,hand,Sample(3),state,out,h),"history crossing");
    const auto before=out.fore[1].r;
    Check(Near(before,rig::Rotate(rest.fore[1].r,{0,1,0},arm::Pi/3)),"wrapped positive roll remains positive at bound");
    Targets(rest,hand,0);
    Check(Deform(rest,hand,Sample(3),state,out,h) && Near(out.fore[1].r,before),"repeat source cannot advance wrist history");
    Check(!Deform(rest,hand,Sample(1),state,out,h),"old source rejected");
    auto s=Sample(4); ++s.handGeneration[0]; Targets(rest,hand,-179*arm::Pi/180);
    Check(Deform(rest,hand,s,state,out,h) && Near(out.fore[1].r,rig::Rotate(rest.fore[1].r,{0,1,0},-arm::Pi/3)),"tracking epoch resets branch");
    rig::State bodyState; Targets(rest,hand,0);
    Check(Deform(rest,hand,Sample(),bodyState,out,h),"body-history baseline");
    const auto elbow=h.elbow;
    const auto body=rig::RotatePose(weapon::Pose{}, {},{0,0,1},arm::Pi);
    auto turned=rest;
    for(auto& p:turned.upper) p=weapon::Compose(p,body);
    for(auto& p:turned.fore) p=weapon::Compose(p,body);
    for(auto& p:turned.hand) p=weapon::Compose(p,body);
    turned.cap=weapon::Compose(turned.cap,body);
    for(int i=0;i<16;++i) hand[i]=turned.hand[i];
    Check(rig::Deform(turned,hand,{},body.Direction({1,-1,0}),body.Direction({1,1,0}),true,Key(),Sample(2),0,arm::Pi/2,bodyState,out,h,body),"body turn transports pole history");
    Check(Near(h.elbow,body.Point(elbow)),"body rotation does not leave elbow hint behind in world space");
}
void Torso() {
    weapon::Pose body;
    for(int i=0;i<360;++i) {
        const float a=i*arm::Pi/180;
        Check(rig::Torso({std::cos(a),std::sin(a),0},{0,0,1},{std::sin(a),-std::cos(a),0},body) && weapon::Rigid(body),"body yaw keeps shoulder frame rigid");
        Check(std::fabs(tracking::DotProduct(body.r,body.u))<1e-5f,"shoulder axes perpendicular");
    }
    Check(rig::Torso({0,0,1},{0,0,1},{1,0,0},body) && Near(body.r,{1,0,0}),"vertical forward uses right-axis fallback");
    Check(!rig::Torso({},{},{},body),"missing body frame rejected");
}
int Replay(bool poses) {
    int count=0; if(!(std::cin>>count) || count<1 || count>512) return 2;
    std::vector<rig::Identity> ids(count);
    std::vector<weapon::Pose> transforms(count);
    for(int i=0;i<count;++i) {
        std::string name; if(!(std::cin>>ids[i].parent) || !std::getline(std::cin>>std::ws,name)) return 2;
        ids[i].name=rig::NameId(name.c_str());
        if(poses) {
            auto& p=transforms[i];
            if(!(std::cin>>p.r.x>>p.r.y>>p.r.z>>p.u.x>>p.u.y>>p.u.z>>p.b.x>>p.b.y>>p.b.z>>p.p.x>>p.p.y>>p.p.z)) return 2;
        }
    }
    rig::Roles left,right;
    const bool ok=rig::Resolve(ids.data(),count,0,left) && rig::Resolve(ids.data(),count,1,right);
    int stages=0;
    if(ok && poses) for(const auto& roles:{left,right}) {
        rig::Arm rest,out; rig::State state; rig::Hinge h; weapon::Pose hands[16];
        for(int i=0;i<3;++i) rest.upper[i]=transforms[roles.upper[i]];
        for(int i=0;i<5;++i) rest.fore[i]=transforms[roles.fore[i]];
        for(int i=0;i<16;++i) rest.hand[i]=transforms[roles.hand[i]];
        rest.cap=transforms[roles.cap];
        const auto s=rest.upper[0].p,e=rest.fore[0].p,w=rest.hand[0].p;
        const auto hint=tracking::Subtract(e,s), fallback=tracking::Subtract(w,s);
        tracking::Vec3 axis{}; if(!arm::Unit(tracking::Subtract(w,e),axis)) return 1;
        for(int turn=0;turn<2;++turn) {
            for(int i=0;i<16;++i) hands[i]=rig::RotatePose(rest.hand[i],w,axis,turn*arm::Pi/4);
            if(!rig::Deform(rest,hands,s,hint,fallback,true,Key(),Sample(turn+1),0,arm::Pi/2,state,out,h)) return 1;
            if(!Near(out.hand[0].p,w) || !Near(out.hand[0].r,hands[0].r)) return 1;
            ++stages;
        }
    }
    std::printf("asset-replay count=%d mapped=%d stages=%d L=%d/%d/%d/cap%d R=%d/%d/%d/cap%d\n",count,ok,stages,left.upper[0],left.fore[0],left.hand[0],left.cap,right.upper[0],right.fore[0],right.hand[0],right.cap);
    return ok?0:1;
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!std::strcmp(argv[1],"mapping")) Mapping();
    else if(!std::strcmp(argv[1],"joints")) Joints();
    else if(!std::strcmp(argv[1],"singular")) Singular();
    else if(!std::strcmp(argv[1],"wrist")) Wrist();
    else if(!std::strcmp(argv[1],"cap")) Cap();
    else if(!std::strcmp(argv[1],"mesh")) Mesh();
    else if(!std::strcmp(argv[1],"staging")) Staging();
    else if(!std::strcmp(argv[1],"history")) History();
    else if(!std::strcmp(argv[1],"torso")) Torso();
    else if(!std::strcmp(argv[1],"replay")) return Replay(false);
    else if(!std::strcmp(argv[1],"replay_pose")) return Replay(true);
    else return 2;
    std::printf("%s: %d checks, %d failures\n",argv[1],checks,failures); return failures?1:0;
}
