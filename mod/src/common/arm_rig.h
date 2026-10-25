#pragma once
#include "weapon_pose.h"
#include <array>

namespace fc2vr::rig {
using weapon::Pose;
using tracking::Vec3;
using tracking::Add;
using tracking::Subtract;
using tracking::Scale;
using tracking::DotProduct;
constexpr std::uint32_t NameId(const char* name) {
    std::uint32_t crc=0xffffffffu;
    for(;*name;++name) {
        crc^=static_cast<unsigned char>(*name);
        for(int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320u:0u);
    }
    return ~crc;
}
struct Identity { std::uint32_t name=0; int parent=-1; };
struct Roles {
    int clavicle=-1, upper[3]{-1,-1,-1}, fore[5]{-1,-1,-1,-1,-1}, hand[16]{}, cap=-1;
    std::array<int,25> Indices() const {
        std::array<int,25> out{}; int n=0;
        for(int i:upper) out[n++]=i;
        for(int i:fore) out[n++]=i;
        for(int i:hand) out[n++]=i;
        out[n]=cap; return out;
    }
};
// Names are case-sensitive CRC32, parent is the actual array index. Never use
// asset order, helper proximity, skin-palette slots or a global index subtraction.
inline bool Resolve(const Identity* ids,int count,int side,Roles& out) {
    if(!ids || count<1 || count>512 || side<0 || side>1) return false;
    for(int i=0;i<count;++i) {
        if(ids[i].parent < -1 || ids[i].parent>=count || ids[i].parent==i) return false;
    }
    for(int i=0;i<count;++i) {
        int j=i;
        for(int depth=0;j>=0;++depth) { if(depth>=count) return false; j=ids[j].parent; }
    }
    auto find=[&](const char* suffix,bool sided=true) {
        char name[64]{}; int n=0;
        if(sided) { name[n++]=side?'R':'L'; name[n++]=' '; }
        while(*suffix && n<63) name[n++]=*suffix++;
        const auto hash=NameId(name); int match=-1;
        for(int i=0;i<count;++i) if(ids[i].name==hash) { if(match>=0) return -1; match=i; }
        return match;
    };
    auto parent=[&](int child,int expected) { return child>=0 && expected>=0 && ids[child].parent==expected; };
    Roles a; a.clavicle=find("Clavicle"); a.upper[0]=find("UpperArm"); a.fore[0]=find("Forearm"); a.hand[0]=find("Hand"); a.cap=find("Elbow");
    if(!parent(a.clavicle,find("Spine2",false)) || !parent(a.upper[0],a.clavicle)
        || !parent(a.fore[0],a.upper[0]) || !parent(a.hand[0],a.fore[0]) || !parent(a.cap,find("Pelvis",false))) return false;
    a.upper[1]=find("U UpperArm twist"); a.upper[2]=find("D UpperArm twist");
    const char* fore[]={"D Forearm twist U","M Forearm twist","U Forearm twist D","B Forearm twist"};
    for(int i=0;i<4;++i) { a.fore[i+1]=find(fore[i]); if(!parent(a.fore[i+1],a.fore[0])) return false; }
    for(int i=1;i<3;++i) if(!parent(a.upper[i],a.upper[0])) return false;
    const char* digits[]={"Thumb01","Thumb02","Thumb03","Index01","Index02","Index03","Middle01","Middle02","Middle03","Ring01","Ring02","Ring03","Pinky01","Pinky02","Pinky03"};
    for(int i=0;i<15;++i) { a.hand[i+1]=find(digits[i]); if(!parent(a.hand[i+1],i%3==0?a.hand[0]:a.hand[i])) return false; }
    const auto owned=a.Indices();
    for(int i=0;i<25;++i) for(int j=0;j<i;++j) if(owned[i]==owned[j]) return false;
    out=a; return true;
}
inline Vec3 Rotate(Vec3 v,Vec3 axis,float angle) {
    return Add(Add(Scale(v,std::cos(angle)),Scale(arm::Cross(axis,v),std::sin(angle))),Scale(axis,DotProduct(axis,v)*(1-std::cos(angle))));
}
inline Pose RotatePose(Pose p,Vec3 pivot,Vec3 axis,float angle) {
    p.r=Rotate(p.r,axis,angle); p.u=Rotate(p.u,axis,angle); p.b=Rotate(p.b,axis,angle);
    p.p=Add(pivot,Rotate(Subtract(p.p,pivot),axis,angle)); return p;
}
inline bool Swing(const Pose& rest,Vec3 start,Vec3 end,Vec3 newStart,Vec3 newEnd,Vec3 hint,Pose& out) {
    Vec3 axis; float angle=0;
    if(!weapon::Rigid(rest) || !arm::Finite(start) || !arm::Finite(newStart)
        || !weapon::FromTo(Subtract(end,start),Subtract(newEnd,newStart),hint,axis,angle)) return false;
    Pose p=RotatePose(rest,start,axis,angle); p.p=Add(p.p,Subtract(newStart,start));
    if(!weapon::Rigid(p)) return false;
    out=p; return true;
}
inline bool Plane(Vec3 preferred,Vec3 axis,Vec3& out) {
    return arm::Unit(Subtract(preferred,Scale(axis,DotProduct(preferred,axis))),out);
}
inline bool Torso(Vec3 forward,Vec3 up,Vec3 rightHint,Pose& out) {
    Vec3 u,f,r;
    if(!arm::Unit(up,u)) return false;
    if(!Plane(forward,u,f)) { if(!Plane(rightHint,u,r)) return false; f=arm::Cross(u,r); }
    if(!arm::Unit(arm::Cross(f,u),r)) return false;
    out={r,u,Scale(f,-1),{}}; return weapon::Rigid(out);
}
struct Hinge { Vec3 shoulder{},elbow{},wrist{},pole{}; };
inline bool SolveHinge(Vec3 shoulder,Vec3 wrist,float a,float b,Vec3 preferred,Vec3 previous,Vec3 restHint,Vec3 fallback,bool keepWrist,Hinge& out) {
    if(!arm::Finite(shoulder) || !arm::Finite(wrist) || !std::isfinite(a) || !std::isfinite(b) || a<1e-4f || b<1e-4f || a>100 || b>100) return false;
    Vec3 direction; const Vec3 delta=Subtract(wrist,shoulder); float distance=std::sqrt(DotProduct(delta,delta));
    if(!std::isfinite(distance)) return false;
    if(!arm::Unit(delta,direction) && !arm::Unit(fallback,direction)) return false;
    const float eps=0.005f*(a<b?a:b), low=std::fabs(a-b)+eps, high=a+b-eps;
    if(!(low<high)) return false;
    const float reach=distance<low?low:distance>high?high:distance;
    if(keepWrist) shoulder=Subtract(wrist,Scale(direction,reach));
    else wrist=Add(shoulder,Scale(direction,reach));
    Vec3 oldPole{},pole{};
    const bool hadPole=Plane(previous,direction,oldPole);
    // Near the pole singularity keep the previous/rest hemisphere; never let an
    // atan2/normalization zero invent an elbow at the wrong radius.
    Vec3 projected=Subtract(preferred,Scale(direction,DotProduct(preferred,direction)));
    const float preferredLength=DotProduct(preferred,preferred), projectedLength=DotProduct(projected,projected);
    bool chosen=arm::Finite(projected) && preferredLength>1e-8f && projectedLength>preferredLength*0.0025f && arm::Unit(projected,pole);
    if(!chosen && hadPole) { pole=oldPole; chosen=true; }
    if(!chosen) chosen=Plane(restHint,direction,pole);
    if(!chosen) chosen=Plane(std::fabs(direction.x)<0.8f?Vec3{1,0,0}:Vec3{0,1,0},direction,pole);
    if(!chosen) return false;
    if(hadPole && DotProduct(pole,oldPole)<0) pole=Scale(pole,-1);
    const double x=(double(a)*a+double(reach)*reach-double(b)*b)/(2*reach);
    const double h2=double(a)*a-x*x;
    if(!std::isfinite(x) || !std::isfinite(h2) || h2 < -1e-5) return false;
    const Vec3 elbow=Add(Add(shoulder,Scale(direction,static_cast<float>(x))),Scale(pole,static_cast<float>(std::sqrt(h2>0?h2:0))));
    if(!arm::Finite(elbow)) return false;
    out={shoulder,elbow,wrist,pole}; return true;
}
inline bool HingeFrame(Vec3 shoulder,Vec3 elbow,Vec3 wrist,Vec3 pole,Pose& out) {
    Vec3 axis,normal,reference{};
    if(!arm::Unit(Subtract(elbow,shoulder),axis)) return false;
    const bool hasReference=arm::Unit(arm::Cross(axis,pole),reference);
    if(!arm::Unit(arm::Cross(axis,Subtract(wrist,elbow)),normal)
        && !arm::Unit(arm::Cross(axis,pole),normal)
        && !arm::Unit(arm::Cross(axis,std::fabs(axis.x)<0.8f?Vec3{1,0,0}:Vec3{0,1,0}),normal)) return false;
    if(hasReference && DotProduct(normal,reference)<0) normal=Scale(normal,-1);
    out={axis,arm::Cross(normal,axis),normal,elbow}; return weapon::Rigid(out);
}
// Closest axial rotation of the full palm residual. Two transverse vectors avoid
// misinterpreting wrist flex as twist when a single palm row approaches the axis.
inline bool WristResidual(const Pose& swung,const Pose& target,Vec3 axis,float& out) {
    Vec3 a,u;
    if(!weapon::Rigid(swung) || !weapon::Rigid(target) || !arm::Unit(axis,a)) return false;
    if(DotProduct(arm::Cross(swung.r,swung.u),swung.b)*DotProduct(arm::Cross(target.r,target.u),target.b)<0.9f) return false;
    if(!arm::Unit(arm::Cross(a,std::fabs(a.x)<0.8f?Vec3{1,0,0}:Vec3{0,1,0}),u)) return false;
    const Vec3 v=arm::Cross(a,u), ru=target.Direction(swung.LocalDirection(u)), rv=target.Direction(swung.LocalDirection(v));
    const float sine=DotProduct(ru,v)-DotProduct(rv,u), cosine=DotProduct(ru,u)+DotProduct(rv,v);
    if(!std::isfinite(sine) || !std::isfinite(cosine) || sine*sine+cosine*cosine<1e-8f) return false;
    out=std::atan2(sine,cosine); return true;
}
struct Arm { Pose upper[3]{},fore[5]{},hand[16]{},cap{}; };
struct State {
    arm::RigKey key{}; authority::Stamp source{}; bool have=false;
    Vec3 pole{}; arm::TwistHistory twist; // neutral body-local pole, not a world-space direction
    void Clear() { have=false; twist.Clear(); pole={}; }
};
// Stage all 25 records before the caller copies any to its output palette. Main
// upper arm only swings; wrist pronation is distributed through named forearm
// helpers by longitudinal position. The palm/weapon basis is never rewritten.
inline bool Deform(const Arm& rest,const Pose (&targetHand)[16],Vec3 shoulder,Vec3 preferred,Vec3 fallback,bool keepWrist,
                   const arm::RigKey& key,const authority::Stamp& sample,int side,float twistLimit,State& state,Arm& out,Hinge& joints,const Pose& body=Pose{}) {
    if(side<0 || side>1 || !std::isfinite(twistLimit) || twistLimit<0 || twistLimit>arm::Pi
        || !authority::Fresh(sample,sample,sample.receivedMs,side) || !weapon::Rigid(body)) return false;
    if(state.have && state.key==key && sample.generation==state.source.generation && sample.sequence<state.source.sequence) return false;
    State next=state;
    const bool same=state.have && state.key==key && authority::SameContext(state.source,sample,side)
        && sample.receivedMs-state.source.receivedMs<=authority::MaxAgeMs;
    if(!same) next.Clear();
    for(const auto& p:rest.upper) if(!weapon::Rigid(p)) return false;
    for(const auto& p:rest.fore) if(!weapon::Rigid(p)) return false;
    for(const auto& p:rest.hand) if(!weapon::Rigid(p)) return false;
    for(const auto& p:targetHand) if(!weapon::Rigid(p)) return false;
    if(!weapon::Rigid(rest.cap)) return false;
    const Vec3 s=rest.upper[0].p,e=rest.fore[0].p,w=rest.hand[0].p;
    const float a=std::sqrt(DotProduct(Subtract(e,s),Subtract(e,s))), b=std::sqrt(DotProduct(Subtract(w,e),Subtract(w,e)));
    Hinge hinge;
    if(!SolveHinge(shoulder,targetHand[0].p,a,b,preferred,same?body.Direction(state.pole):Vec3{},Subtract(e,s),fallback,keepWrist,hinge)) return false;
    Arm stage; Pose swungPalm;
    for(int i=0;i<3;++i) if(!Swing(rest.upper[i],s,e,hinge.shoulder,hinge.elbow,hinge.pole,stage.upper[i])) return false;
    for(int i=0;i<5;++i) if(!Swing(rest.fore[i],e,w,hinge.elbow,hinge.wrist,hinge.pole,stage.fore[i])) return false;
    if(!Swing(rest.hand[0],e,w,hinge.elbow,hinge.wrist,hinge.pole,swungPalm)) return false;
    float measured=0,continuous=0;
    const Vec3 axisVector=Subtract(hinge.wrist,hinge.elbow); Vec3 axis;
    if(!arm::Unit(axisVector,axis) || !WristResidual(swungPalm,targetHand[0],axis,measured)
        || !next.twist.Resolve(measured,key,sample,side,true,continuous)) return false;
    const float applied=continuous>twistLimit?twistLimit:continuous < -twistLimit?-twistLimit:continuous;
    for(int i=1;i<5;++i) {
        const float along=DotProduct(Subtract(stage.fore[i].p,hinge.elbow),axis)/b;
        if(!std::isfinite(along) || along < -0.05f || along>1.05f) return false;
        const float weight=along<0?0:along>1?1:along;
        stage.fore[i]=RotatePose(stage.fore[i],hinge.elbow,axis,applied*weight);
    }
    const Vec3 shift=Subtract(hinge.wrist,targetHand[0].p);
    for(int i=0;i<16;++i) { stage.hand[i]=targetHand[i]; stage.hand[i].p=Add(stage.hand[i].p,shift); }
    Pose oldHinge,newHinge;
    Vec3 oldAxis; if(!arm::Unit(Subtract(e,s),oldAxis)) return false;
    const Vec3 references[]={rest.upper[0].r,rest.upper[0].u,rest.upper[0].b};
    int best=0; for(int i=1;i<3;++i) if(std::fabs(DotProduct(oldAxis,references[i]))<std::fabs(DotProduct(oldAxis,references[best]))) best=i;
    const Vec3 oldHint=references[best], newHint=stage.upper[0].Direction(rest.upper[0].LocalDirection(oldHint));
    if(!HingeFrame(s,e,w,oldHint,oldHinge) || !HingeFrame(hinge.shoulder,hinge.elbow,hinge.wrist,newHint,newHinge)) return false;
    stage.cap=weapon::Compose(weapon::Relative(rest.cap,oldHinge),newHinge);
    for(const auto& p:stage.upper) if(!weapon::Rigid(p)) return false;
    for(const auto& p:stage.fore) if(!weapon::Rigid(p)) return false;
    for(const auto& p:stage.hand) if(!weapon::Rigid(p)) return false;
    if(!weapon::Rigid(stage.cap)) return false;
    if(!same || sample.sequence!=state.source.sequence) next.pole=body.LocalDirection(hinge.pole);
    next.have=true; next.key=key; next.source=sample; state=next; out=stage; joints=hinge; return true;
}
} // namespace fc2vr::rig
