#pragma once
#include <cmath>
namespace fc2vr::projectile {
struct Aim {float origin[3]{},direction[3]{};bool hitAllowed=false;};
inline bool Resolve(bool left,const float* nativeOrigin,const float* barrelOrigin,
    const float* sightDirection,const float* target,Aim& result){
    const float* origin=left?barrelOrigin:nativeOrigin;
    float forward[3],delta[3],length=0,forwardLength=0;
    for(int i=0;i<3;++i){
        if(!std::isfinite(origin[i]) || !std::isfinite(target[i]) || !std::isfinite(sightDirection[i]))return false;
        forward[i]=sightDirection[i];delta[i]=target[i]-origin[i];
        length+=delta[i]*delta[i];forwardLength+=forward[i]*forward[i];
    }
    if(!std::isfinite(length) || !std::isfinite(forwardLength) || forwardLength<.000001f)return false;
    forwardLength=std::sqrt(forwardLength);for(float& v:forward)v/=forwardLength;
    if(length<.000001f && !left)return false; // preserve native right-hand behavior
    float alignment=-1;
    if(length>=.000001f){length=std::sqrt(length);alignment=0;for(int i=0;i<3;++i){delta[i]/=length;alignment+=delta[i]*forward[i];}}
    const bool aligned=alignment>=.9f;
    for(int i=0;i<3;++i){result.origin[i]=origin[i];result.direction[i]=aligned?delta[i]:forward[i];}
    // A behind/near-barrel scope target must not remain a forced impact point.
    result.hitAllowed=!left || aligned;
    return true;
}
}
