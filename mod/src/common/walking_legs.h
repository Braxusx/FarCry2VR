#pragma once
#include "pose_frame.h"
#include <algorithm>

namespace fc2vr::walking {
using tracking::Vec3;
using tracking::Add;
using tracking::Subtract;
using tracking::Scale;
using tracking::DotProduct;
inline float Length(Vec3 v) { return std::sqrt(DotProduct(v,v)); }
inline bool Finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline Vec3 Unit(Vec3 v,Vec3 fallback={0,1,0}) { float n=Length(v); return n>1e-6f?Scale(v,1/n):fallback; }
inline float Horizontal(Vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y); }
inline Vec3 Lerp(Vec3 a,Vec3 b,float t) { return Add(a,Scale(Subtract(b,a),t)); }
struct Ground { bool valid=false; Vec3 point{},normal{0,0,1}; };
inline bool Height(const Ground& ground,Vec3 at,float& z) {
    if(!ground.valid || !Finite(ground.point) || !Finite(ground.normal) || ground.normal.z<.55f
        || Horizontal(Subtract(at,ground.point))>.65f) return false;
    z=ground.point.z-(ground.normal.x*(at.x-ground.point.x)+ground.normal.y*(at.y-ground.point.y))/ground.normal.z;
    return std::isfinite(z);
}
struct Input {
    bool allowed=false;
    uintptr_t owner=0; uint64_t generation=0;
    double seconds=0;
    Vec3 root{},forward{0,1,0},ankle[2]{};
    float clearance[2]={.15f,.15f}; Ground ground[2];
};
struct Output {
    bool valid=false; Vec3 ankle[2]{},forward[2]{};
    int swinging=-1; unsigned steps=0;
};
// Visual-only foot planner. Actual root displacement, including room-scale
// movement, drives cadence; pressing a stick against a wall cannot walk in place.
// One foot swings at a time; the other retains its world position.
class Planner {
    bool have=false;
    uintptr_t owner=0; uint64_t generation=0;
    double last=0;
    Vec3 root{},velocity{},feet[2]{},facing[2]{},from{},to{},fromForward{};
    int swing=-1,next=0; float phase=0,duration=.25f; unsigned steps=0;
    Output Result() const {
        Output o; o.valid=have;o.swinging=swing;o.steps=steps;
        for(int i=0;i<2;++i) { o.ankle[i]=feet[i];o.forward[i]=facing[i]; }
        return o;
    }
public:
    void Reset() { *this=Planner{}; }
    Output Update(const Input& in) {
        Vec3 desired[2];
        if(!in.allowed || !in.owner || !std::isfinite(in.seconds) || !Finite(in.root) || !Finite(in.forward)) { Reset();return {}; }
        for(int i=0;i<2;++i) {
            if(!Finite(in.ankle[i]) || !std::isfinite(in.clearance[i]) || in.clearance[i]<.02f || in.clearance[i]>.35f) { Reset();return {}; }
            float height;
            if(!Height(in.ground[i],in.ankle[i],height) || std::fabs(height+in.clearance[i]-in.ankle[i].z)>.45f) { Reset();return {}; }
            desired[i]=in.ankle[i];desired[i].z=height+in.clearance[i];
        }
        Vec3 forward=Unit({in.forward.x,in.forward.y,0});
        const float dt=float(in.seconds-last);
        Vec3 delta=Subtract(in.root,root);
        if(!have || owner!=in.owner || generation!=in.generation || dt<0 || dt>.2f || Length(delta)>.6f) {
            Reset();have=true;owner=in.owner;generation=in.generation;last=in.seconds;root=in.root;
            for(int i=0;i<2;++i) { feet[i]=desired[i];facing[i]=forward; }
            return Result();
        }
        if(dt<.0001f) return Result(); // repeated hierarchy call must not advance twice
        last=in.seconds;root=in.root;delta.z=0;
        const Vec3 raw=Scale(delta,1/dt);
        if(Length(raw)>8) { Reset();return {}; }
        velocity=Lerp(velocity,raw,1-std::exp(-dt/.07f));
        const float speed=Horizontal(velocity);
        if(swing>=0) {
            phase=std::min(1.f,phase+dt/duration);
            const float u=phase*phase*(3-2*phase);
            feet[swing]=Lerp(from,to,u);
            feet[swing].z+=4*phase*(1-phase)*std::min(.10f,.045f+speed*.012f);
            const float fromYaw=std::atan2(fromForward.y,fromForward.x);
            const float turn=std::atan2(fromForward.x*forward.y-fromForward.y*forward.x,DotProduct(fromForward,forward));
            facing[swing]={std::cos(fromYaw+turn*u),std::sin(fromYaw+turn*u),0};
            if(phase>=1) { feet[swing]=to;facing[swing]=forward;next=1-swing;swing=-1; }
        }
        if(swing<0) {
            float error[2]; bool needs[2];
            const float threshold=.08f+std::min(.02f,speed*.01f);
            for(int i=0;i<2;++i) {
                error[i]=Horizontal(Subtract(desired[i],feet[i]));
                needs[i]=error[i]>threshold || std::fabs(desired[i].z-feet[i].z)>.07f
                    || DotProduct(forward,facing[i])<.82f;
            }
            int chosen=needs[next]?next:needs[1-next]?1-next:-1;
            if(chosen>=0) {
                // Use immediate motion for cadence at startup; smoothing this
                // decision leaves the stance leg stranded behind a fast start.
                const float instantSpeed=Length(raw);
                duration=std::clamp(.38f/(std::max(speed,instantSpeed)+.8f),.12f,.32f);
                if(instantSpeed>.1f) {
                    // A short first step, then longer steps as the opposite foot
                    // lands ahead. Finish before that stance foot exceeds reach.
                    const float behind=DotProduct(Subtract(desired[1-chosen],feet[1-chosen]),Scale(raw,1/instantSpeed));
                    duration=std::min(duration,std::max(.04f,(.28f-behind)/instantSpeed));
                }
                Vec3 lead=Scale(raw,duration);
                if(instantSpeed>.15f) lead=Add(lead,Scale(raw,std::min(.20f,instantSpeed*.12f)/instantSpeed));
                const float len=Length(lead);if(len>.55f) lead=Scale(lead,.55f/len);
                to=Add(desired[chosen],lead);
                float height;
                if(!Height(in.ground[chosen],to,height)) { Reset();return {}; }
                to.z=height+in.clearance[chosen];
                from=feet[chosen];fromForward=facing[chosen];phase=0;swing=chosen;++steps;
            }
        }
        return Result();
    }
};
struct JointSolution { bool valid=false; Vec3 knee{},ankle{}; };
inline JointSolution Solve(Vec3 hip,Vec3 target,Vec3 bend,float upper,float lower) {
    if(!Finite(hip)||!Finite(target)||!Finite(bend)||!std::isfinite(upper)||!std::isfinite(lower)
        || upper<.1f||lower<.1f||upper>1||lower>1) return {};
    Vec3 offset=Subtract(target,hip);float reach=Length(offset);
    if(reach<1e-5f) return {};
    Vec3 direction=Scale(offset,1/reach);
    const float distance=std::clamp(reach,std::fabs(upper-lower)+.002f,(upper+lower)*.995f);
    Vec3 pole=Subtract(bend,Scale(direction,DotProduct(bend,direction)));
    if(Length(pole)<.01f) {
        Vec3 fallback=std::fabs(direction.x)<.8f?Vec3{1,0,0}:Vec3{0,1,0};
        pole=Subtract(fallback,Scale(direction,DotProduct(fallback,direction)));
    }
    pole=Unit(pole);
    float along=(upper*upper-lower*lower+distance*distance)/(2*distance);
    float height=std::sqrt(std::max(0.f,upper*upper-along*along));
    return {true,Add(Add(hip,Scale(direction,along)),Scale(pole,height)),Add(hip,Scale(direction,distance))};
}
}
