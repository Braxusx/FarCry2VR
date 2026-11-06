#pragma once
#include <cmath>

namespace fc2vr::shoulder {
inline bool Basis(float* forward,const float* up,float* right) {
    float dot=0;for(int i=0;i<3;++i)dot+=forward[i]*up[i];
    float length=0;for(int i=0;i<3;++i){forward[i]-=up[i]*dot;length+=forward[i]*forward[i];}
    if(!std::isfinite(length) || length<0.0001f)return false;
    length=std::sqrt(length);for(int i=0;i<3;++i)forward[i]/=length;
    right[0]=forward[1]*up[2]-forward[2]*up[1];
    right[1]=forward[2]*up[0]-forward[0]*up[2];
    right[2]=forward[0]*up[1]-forward[1]*up[0];
    length=0;for(int i=0;i<3;++i)length+=right[i]*right[i];
    if(!std::isfinite(length) || length<0.0001f)return false;
    length=std::sqrt(length);for(int i=0;i<3;++i)right[i]/=length;
    // The cross product defines anatomical right through a full turn. A body-
    // hemisphere sign correction would exchange shoulders beyond +/-90 deg.
    return true;
}
}
