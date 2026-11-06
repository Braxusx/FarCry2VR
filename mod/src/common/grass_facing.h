#pragma once
#include <cmath>
#include <initializer_list>

namespace fc2vr::grass {
// Captured Steam DX10 _GrassCylindricalBillboardMatrix. Only its horizontal
// output rows are changed; roots, vertical scale, wind and projection are not.
inline bool SetWorldYaw(float* m,float yaw) {
    if(!m || !std::isfinite(yaw))return false;
    for(int i=0;i<16;++i)if(!std::isfinite(m[i]))return false;
    const float a=m[0],b=m[1],c=m[4],d=m[5];
    if(std::fabs(a*a+b*b-1)>0.025f || std::fabs(c*c+d*d-1)>0.025f ||
       std::fabs(a*c+b*d)>0.025f || std::fabs(a*d-b*c-1)>0.025f)return false;
    for(int i: {2,3,6,7,8,9,11,12,13,14})if(std::fabs(m[i])>0.0001f)return false;
    if(m[10]<0.1f || m[10]>10 || std::fabs(m[15]-1)>0.0001f)return false;
    const float co=std::cos(yaw),si=std::sin(yaw);
    m[0]=co;m[1]=-si;m[4]=si;m[5]=co;
    return true;
}
inline bool RemoveHeadYaw(float* m,float yaw) {
    return m && std::isfinite(yaw) && SetWorldYaw(m,std::atan2(m[4],m[0])-yaw);
}
}
