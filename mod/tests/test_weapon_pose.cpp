#include "common/weapon_pose.h"
#include <cstdio>
#include <cstring>
#include <limits>
using namespace fc2vr;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
bool Near(tracking::Vec3 a,tracking::Vec3 b,float eps=2e-4f) { const auto d=tracking::Subtract(a,b); return tracking::DotProduct(d,d)<eps*eps; }
authority::Stamp Sample(std::uint64_t i=1) { authority::Stamp s; s.sequence=i; s.receivedMs=1000+i*10; s.generation=2; s.handGeneration[0]=3; s.handGeneration[1]=4; s.focused=s.synced=true; return s; }
weapon::SupportKey Key() {
    weapon::SupportKey k; k.rig.pawn=1; k.rig.skeleton=2; k.rig.palette=3; k.rig.weapon=4; k.rig.count=120;
    k.rig.upper=10; k.rig.fore=11; k.rig.hand=12; k.model=5; k.palette=6; k.modelCount=20; k.definition=7; k.hold=8; return k;
}
weapon::Pose Yaw(float a,tracking::Vec3 p={}) { return {{std::cos(a),std::sin(a),0},{-std::sin(a),std::cos(a),0},{0,0,1},p}; }
void Socket() {
    weapon::SupportBinding binding; const auto key=Key(); const auto accepted=Yaw(0.3f,{0.2f,0.1f,-0.4f});
    weapon::Pose socket,right; std::uint64_t token=0,original=0;
    Check(!binding.Read(key,Sample(),1010,socket,right,token),"unbound socket unavailable");
    Check(binding.Publish(key,Sample(),accepted,{},true),"accepted hold binds socket");
    Check(binding.Read(key,Sample(),1010,socket,right,token),"bound descriptor readable"); original=token;
    for(int i=2;i<502;++i) {
        const auto animation=Yaw(i*0.03f,{i*0.01f,-i*0.02f,i*0.03f});
        Check(binding.Publish(key,Sample(i),animation,{},true),"same owner publication");
        Check(binding.Read(key,Sample(i),1000+i*10,socket,right,token),"current descriptor readable");
        Check(token==original && Near(socket.p,accepted.p) && Near(socket.r,accepted.r),"animated wrist cannot renew socket");
        const auto currentRight=Yaw(i*0.05f,{i*0.1f,2,3});
        const auto world=weapon::Compose(socket,currentRight);
        const auto expected=currentRight.Point(accepted.p);
        Check(Near(world.p,expected),"current physical root places socket");
        const auto local=weapon::Relative(world,currentRight);
        Check(Near(local.p,accepted.p) && Near(local.u,accepted.u),"moving root cancels in relative socket");
    }
    Check(!binding.Read(key,Sample(501),6111,socket,right,token),"stale producer cannot authorize grip");
    Check(!binding.Publish(key,Sample(500),{}, {},true),"out-of-order support publication rejected");
    Check(binding.Read(key,Sample(501),6010,socket,right,token) && token==original,"older call cannot replace descriptor");
}
void Ownership() {
    for(int reason=0;reason<19;++reason) {
        weapon::SupportBinding b; auto k=Key(); auto s=Sample(); weapon::Pose socket,right; std::uint64_t t=0;
        b.Publish(k,s,Yaw(0,{0.2f,0,0}),{},true); b.Read(k,s,1010,socket,right,t); const auto old=t;
        s=Sample(2);
        if(reason==0) ++k.rig.pawn;
        if(reason==1) ++k.rig.skeleton;
        if(reason==2) ++k.rig.palette;
        if(reason==3) ++k.rig.weapon;
        if(reason==4) ++k.rig.count;
        if(reason==5) ++k.rig.upper;
        if(reason==6) ++k.rig.fore;
        if(reason==7) ++k.rig.hand;
        if(reason==8) ++k.rig.mode;
        if(reason==9) ++k.model;
        if(reason==10) ++k.palette;
        if(reason==11) ++k.modelCount;
        if(reason==12) ++k.definition;
        if(reason==13) ++k.hold;
        if(reason==14) ++s.generation;
        if(reason==15) ++s.handGeneration[0];
        if(reason==16) ++s.handGeneration[1];
        if(reason==17) s.receivedMs+=200;
        if(reason==18) b.Clear();
        const auto replacement=Yaw(0.7f,{0,0.5f,0});
        Check(b.Publish(k,s,replacement,{},true),"new context binds independently");
        Check(b.Read(k,s,s.receivedMs,socket,right,t) && t!=old && Near(socket.p,replacement.p),"context changes descriptor token");
    }
    weapon::SupportBinding b; weapon::Pose socket,right; std::uint64_t token=0;
    b.Publish(Key(),Sample(),{}, {},true);
    Check(!b.Publish(Key(),Sample(2),{}, {},false) && !b.Read(Key(),Sample(2),1020,socket,right,token),"disallowed mode clears binding");
    auto invalid=Yaw(0); invalid.r.x=std::numeric_limits<float>::quiet_NaN();
    Check(!b.Publish(Key(),Sample(3),invalid,{},true),"nonfinite hold rejected");
    invalid={}; invalid.r={2,0,0}; Check(!b.Publish(Key(),Sample(3),invalid,{},true),"nonrigid hold rejected");
}
void Latch() {
    for(int reason=0;reason<7;++reason) {
        weapon::SupportLatch l; auto s=Sample();
        Check(!l.Update(true,true,true,1,s),"held-on-start is not a grab");
        s=Sample(2); Check(!l.Update(false,true,true,1,s),"neutral arms grip");
        s=Sample(3); Check(l.Update(true,true,true,1,s),"new near grip acquires");
        s=Sample(4); Check(l.Update(true,false,true,1,s),"held grip does not require near every frame");
        bool allowed=true; std::uint64_t token=1;
        s=Sample(5);
        if(reason==0) ++s.generation;
        if(reason==1) ++s.handGeneration[0];
        if(reason==2) ++s.handGeneration[1];
        if(reason==3) ++token;
        if(reason==4) allowed=false;
        if(reason==5) s.focused=false;
        if(reason==6) s.synced=false;
        Check(!l.Update(true,true,allowed,token,s),"loss/owner change cancels grip");
        s.sequence++; s.receivedMs+=10; s.focused=s.synced=true;
        Check(!l.Update(true,true,true,token,s),"held-through-recovery does not reacquire");
        s.sequence++; s.receivedMs+=10; Check(!l.Update(false,true,true,token,s),"neutral after recovery");
        s.sequence++; s.receivedMs+=10; Check(l.Update(true,true,true,token,s),"fresh grip after recovery");
    }
}
void Arc() {
    tracking::Vec3 axis{}; float angle=0;
    Check(weapon::FromTo({1,0,0},{0,1,0},{0,0,1},axis,angle) && std::fabs(angle-arm::Pi/2)<1e-5f && Near(axis,{0,0,1}),"support quarter turn");
    Check(weapon::FromTo({1,0,0},{-1,0,0},{0,1,0},axis,angle) && std::fabs(angle-arm::Pi)<1e-5f && Near(axis,{0,1,0}),"antiparallel support has stable axis");
    Check(weapon::FromTo({1,0,0},{-1,0,0},{1,0,0},axis,angle) && arm::Finite(axis),"parallel hint has deterministic fallback");
    Check(!weapon::FromTo({0,0,0},{1,0,0},{0,1,0},axis,angle),"zero lever rejected");
    Check(!weapon::FromTo({1,0,0},{0,0,0},{0,1,0},axis,angle),"coincident hands rejected");
    for(int degree=-180;degree<=180;++degree) {
        const float a=degree*arm::Pi/180;
        const tracking::Vec3 lever{0.3f,0,0}, target{std::cos(a)*0.3f,std::sin(a)*0.3f,0};
        Check(weapon::FromTo(lever,target,{0,0,1},axis,angle),"full support arc available");
        const auto rotated=tracking::Add(tracking::Add(tracking::Scale(lever,std::cos(angle)),tracking::Scale(arm::Cross(axis,lever),std::sin(angle))),tracking::Scale(axis,tracking::DotProduct(axis,lever)*(1-std::cos(angle))));
        Check(Near(rotated,target),"support lever reaches requested direction without animation");
        const tracking::Vec3 root{12,3,8};
        Check(Near(tracking::Subtract(tracking::Add(root,rotated),root),target),"steering pivots about fixed right hand");
    }
}
tracking::Vec3 Rotate(tracking::Vec3 v,tracking::Vec3 axis,float angle) {
    return tracking::Add(tracking::Add(tracking::Scale(v,std::cos(angle)),tracking::Scale(arm::Cross(axis,v),std::sin(angle))),tracking::Scale(axis,tracking::DotProduct(axis,v)*(1-std::cos(angle))));
}
weapon::Pose Orientation(float yaw,float pitch,float roll) {
    weapon::Pose p;
    p.r=Rotate(p.r,{0,1,0},yaw); p.u=Rotate(p.u,{0,1,0},yaw); p.b=Rotate(p.b,{0,1,0},yaw);
    p.u=Rotate(p.u,p.r,pitch); p.b=Rotate(p.b,p.r,pitch);
    p.r=Rotate(p.r,p.b,roll); p.u=Rotate(p.u,p.b,roll); return p;
}
void Spread() {
    for(int i=0;i<1440;++i) {
        const auto native=Orientation(i*0.013f,std::sin(i*0.1f)*1.55f,i*0.027f);
        const auto target=Orientation(-i*0.017f,std::cos(i*0.2f)*1.55f,-i*0.031f);
        const tracking::Vec3 raw{std::sin(i*0.03f)*0.08f,std::cos(i*0.07f)*0.05f,-1};
        tracking::Vec3 local{}; Check(arm::Unit(raw,local),"native fixture normalized");
        const auto shot=native.Direction(local), expected=target.Direction(local);
        tracking::Vec3 actual{};
        Check(weapon::TransportSpread(shot,native,target,actual),"yaw/pitch/roll spread transport available");
        Check(Near(actual,expected),"all spread components follow target basis");
        Check(Near(target.LocalDirection(actual),local),"asymmetric native spread preserved in local coordinates");
        Check(std::fabs(tracking::DotProduct(actual,tracking::Scale(target.b,-1))+local.z)<1e-5f,"native cone angle retained");
        Check(std::fabs(tracking::DotProduct(actual,actual)-1)<1e-5f,"result unit length");
        tracking::Vec3 identity{};
        Check(weapon::TransportSpread(shot,native,native,identity) && Near(identity,shot),"identity orientation preserves native shot");
        Check(weapon::TransportSpread(tracking::Scale(native.b,-1),native,target,actual) && Near(actual,tracking::Scale(target.b,-1)),"zero spread follows accepted central ray");
        // Common rigid world motion must cancel from the local spread coordinates.
        const auto world=Orientation(i*0.002f,0.4f,-0.8f);
        const auto n=weapon::Compose(native,world),t=weapon::Compose(target,world);
        Check(weapon::TransportSpread(world.Direction(shot),n,t,actual) && Near(actual,world.Direction(expected)),"transport equivariant under common world rotation");
    }
    const weapon::Pose native{},rolled=Orientation(0,0,arm::Pi/2);
    tracking::Vec3 shot{}; arm::Unit({0.1f,0,-1},shot); tracking::Vec3 correct{};
    Check(weapon::TransportSpread(shot,native,rolled,correct),"roll control available");
    // The old addition leaves the X residual in world X when the weapon rolls.
    const auto legacy=tracking::Add(tracking::Scale(rolled.b,-1),tracking::Subtract(shot,{0,0,-1}));
    tracking::Vec3 old{}; arm::Unit(legacy,old);
    Check(!Near(old,correct,0.05f),"negative control: old world residual fails 90-degree roll");
    tracking::Vec3 scoped{};
    const auto scope=Orientation(0.7f,-0.3f,0);
    Check(weapon::TransportSpread(shot,native,scope,scoped) && Near(scoped,scope.Direction(shot)),"scope uses its rendered basis, not weapon roll");
}
void InvalidSpread() {
    weapon::Pose good; tracking::Vec3 out{7,8,9};
    Check(!weapon::TransportSpread({0,0,0},good,good,out) && Near(out,{7,8,9}),"invalid input leaves caller output unchanged");
    Check(!weapon::TransportSpread({0,std::numeric_limits<float>::infinity(),-1},good,good,out),"nonfinite native shot rejected");
    auto bad=good; bad.r={2,0,0};
    Check(!weapon::TransportSpread({0,0,-1},bad,good,out),"scaled source basis rejected");
    Check(!weapon::TransportSpread({0,0,-1},good,bad,out),"scaled target basis rejected");
    bad=good; bad.u=bad.r; Check(!weapon::TransportSpread({0,0,-1},good,bad,out),"singular basis rejected");
    bad=good; bad.r={-1,0,0}; Check(!weapon::TransportSpread({0,0,-1},good,bad,out),"reflected target cannot mirror spread");
    bad=good; bad.p.x=std::numeric_limits<float>::quiet_NaN();
    Check(!weapon::TransportSpread({0,0,-1},good,bad,out),"nonfinite target origin rejected");
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!std::strcmp(argv[1],"socket")) Socket();
    else if(!std::strcmp(argv[1],"ownership")) Ownership();
    else if(!std::strcmp(argv[1],"latch")) Latch();
    else if(!std::strcmp(argv[1],"arc")) Arc();
    else if(!std::strcmp(argv[1],"spread")) Spread();
    else if(!std::strcmp(argv[1],"invalid_spread")) InvalidSpread();
    else return 2;
    std::printf("%s: %d checks, %d failures\n",argv[1],checks,failures); return failures?1:0;
}
