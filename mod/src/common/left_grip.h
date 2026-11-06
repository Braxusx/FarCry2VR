#pragma once
#include <cmath>

namespace fc2vr::left_grip {
// Native left/right neutral wrists differ by bone-Z and grip-X reflections.
// Two reflections preserve determinant +1: bone identities/mesh winding stay intact.
inline void Mirror(const float* pose,int lateralAxis,float* result) {
    for(int i=0;i<16;++i)result[i]=pose[i];
    for(int row=0;row<4;++row)result[row*4]=-result[row*4];
    for(int column=0;column<4;++column)result[lateralAxis*4+column]=-result[lateralAxis*4+column];
}
inline bool Unit(float* v) {
    const float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if(!std::isfinite(length) || length<0.00001f)return false;
    for(int i=0;i<3;++i)v[i]/=length;
    return true;
}
inline void Cross(const float* a,const float* b,float* out) {
    out[0]=a[1]*b[2]-a[2]*b[1];out[1]=a[2]*b[0]-a[0]*b[2];out[2]=a[0]*b[1]-a[1]*b[0];
}
// The muzzle's local X axis is lateral; its local Y axis is the native firing
// direction. Reflect about the muzzle's center plane, then grip X. This handles
// model pivots/axes that differ from the inventory object's cached axis guess.
inline bool MirrorWeapon(const float* pose,const float* muzzle,float* result) {
    float normal[3]={muzzle[0],muzzle[1],muzzle[2]};if(!Unit(normal))return false;
    const float plane=normal[0]*muzzle[12]+normal[1]*muzzle[13]+normal[2]*muzzle[14];
    if(!std::isfinite(plane))return false;
    float reflection[16]{};reflection[15]=1;
    for(int i=0;i<3;++i){for(int j=0;j<3;++j)reflection[i*4+j]=(i==j?1.0f:0.0f)-2*normal[i]*normal[j];reflection[12+i]=2*plane*normal[i];}
    for(int i=0;i<4;++i)for(int j=0;j<4;++j){float v=0;for(int k=0;k<4;++k)v+=reflection[i*4+k]*pose[k*4+j];result[i*4+j]=(j==0?-v:v);}
    return true;
}
// Rotate native spread onto the actual barrel, preserving its cone angle.
inline bool Spread(const float* camera,const float* shot,const float* barrel,float* out) {
    float a[3],b[3],d[3];for(int i=0;i<3;++i){a[i]=camera[i];b[i]=barrel[i];d[i]=shot[i];}
    if(!Unit(a) || !Unit(b) || !Unit(d))return false;
    const float c=std::fmax(-1.0f,std::fmin(1.0f,a[0]*b[0]+a[1]*b[1]+a[2]*b[2]));
    float axis[3];Cross(a,b,axis);
    const float sine=std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
    if(sine>0.000001f){
        for(int i=0;i<3;++i)axis[i]/=sine;
        float perpendicular[3];Cross(axis,d,perpendicular);
        const float dot=d[0]*axis[0]+d[1]*axis[1]+d[2]*axis[2];
        for(int i=0;i<3;++i)out[i]=d[i]*c+perpendicular[i]*sine+axis[i]*dot*(1-c);
    }else if(c>=0){
        for(int i=0;i<3;++i)out[i]=d[i];
    }else{
        float basis[3]={0,1,0};if(std::fabs(a[1])>0.9f){basis[0]=1;basis[1]=0;}
        Cross(a,basis,axis);if(!Unit(axis))return false;
        const float dot=d[0]*axis[0]+d[1]*axis[1]+d[2]*axis[2];
        for(int i=0;i<3;++i)out[i]=2*dot*axis[i]-d[i];
    }
    return Unit(out);
}
} // namespace fc2vr::left_grip
