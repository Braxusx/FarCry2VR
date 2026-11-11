#pragma once
#include "tracking_frame.h"
#include <cmath>

namespace fc2vr::interaction {
struct Ray { tracking::Vec3 origin{}, direction{}; bool valid=false; };
inline Ray MakeRay(tracking::Vec3 origin,tracking::Vec3 direction,bool allowed) {
    Ray ray;
    if (!allowed || !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) return ray;
    float length=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
    if (!std::isfinite(length) || length<0.5f || length>1.5f) return ray;
    ray={origin,{direction.x/length,direction.y/length,direction.z/length},true}; return ray;
}
// Native aim helpers use a quaternion rotating game +Y onto the aim direction.
inline bool Rotation(const Ray& ray,float q[4]) {
    if (!ray.valid) return false;
    q[0]=ray.direction.z; q[1]=0; q[2]=-ray.direction.x;
    // Avoid cancellation near a physical 180-degree turn. Only the exactly
    // opposite direction needs an arbitrary rotation axis.
    const float transverse=ray.direction.x*ray.direction.x+ray.direction.z*ray.direction.z;
    q[3]=ray.direction.y<0 ? transverse/(1-ray.direction.y) : 1+ray.direction.y;
    if(transverse<1e-12f && ray.direction.y<0) { q[0]=0;q[1]=0;q[2]=1;q[3]=0; }
    float n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    for(int i=0;i<4;++i) q[i]/=n;
    return true;
}
}
