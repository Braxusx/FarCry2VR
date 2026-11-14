#pragma once
#include "pose_frame.h"

namespace fc2vr::tracking {
// Stick release reaches the renderer before the last queued native turn.
// Keep deliberate turning out of sprint-sway smoothing while that queue drains.
class TurnIntent {
    double until=0,last=0;
public:
    bool Update(double now,bool turning,bool eligible) {
        if(!eligible || !std::isfinite(now) || now<last || now-last>250) until=0;
        last=now;
        if(!eligible || !std::isfinite(now)) return false;
        if(turning) until=now+150;
        return turning || now<until;
    }
};
// Transport a stabilized on-foot origin with native locomotion and yaw, without
// reintroducing the animated Camera bone. Physical body-follow is already in
// the tracked controller positions, so its unacknowledged reference-space travel
// must be removed once. This changes presentation, never the native root.
inline bool FollowOnFootBody(GameBasis& game, const float* from, const float* to,
                            Vec3 physicalTravel = {}, const float* lookYawDelta = nullptr) {
    for (unsigned i=0;i<16;++i)
        if (!std::isfinite(from[i]) || !std::isfinite(to[i])) return false;
    const Vec3 movement{to[12]-from[12],to[13]-from[13],to[14]-from[14]};
    const Vec3 offset=Subtract(game.P,{from[12],from[13],from[14]});
    const float a2=from[4]*from[4]+from[5]*from[5],b2=to[4]*to[4]+to[5]*to[5];
    if (DotProduct(movement,movement)>=2.25f || DotProduct(offset,offset)>9
        || a2<1e-8f || b2<1e-8f) return false;
    const float n=std::sqrt(a2*b2);
    if(lookYawDelta && !std::isfinite(*lookYawDelta)) return false;
    const float c=lookYawDelta?std::cos(*lookYawDelta):(from[4]*to[4]+from[5]*to[5])/n;
    const float s=lookYawDelta?std::sin(*lookYawDelta):(from[4]*to[5]-from[5]*to[4])/n;
    auto rotate=[&](Vec3 v) { return Vec3{c*v.x-s*v.y,s*v.x+c*v.y,v.z}; };
    GameBasis next=game;
    next.R=rotate(game.R);next.F=rotate(game.F);
    next.P=Subtract(Add({to[12],to[13],to[14]},rotate(offset)),next.Direction(physicalTravel));
    const Vec3 values[]={next.P,next.R,next.F,physicalTravel};
    for(auto v:values) if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)) return false;
    if(DotProduct(physicalTravel,physicalTravel)>2.25f) return false;
    game=next;return true;
}
}
