#include "common/body_facing.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int bad=0;
    auto check=[&](bool ok,const char* m) { if(!ok) { ++bad; std::fprintf(stderr,"FAIL: %s\n",m); } };
    for(int deg=-180;deg<=180;++deg) {
        float a=deg*.01745329252f;
        Vec3 head{-std::sin(a),std::cos(a),.3f},f{},r{};
        check(HeadFacing(head,f,r),"valid head yaw");
        check(std::fabs(DotProduct(f,r))<1e-6f,"shoulders perpendicular to gaze");
        check(std::fabs(f.x*r.y-f.y*r.x+1)<1e-6f,"right stays right across 90 and 180 degrees");
        const float turn=BodyYawDelta({0,1,0},f);
        check(std::fabs(std::sin(turn)-std::sin(a))<1e-6f && std::fabs(std::cos(turn)-std::cos(a))<1e-6f,"body presentation follows physical yaw without world rotation");
    }
    Vec3 f{},r{};
    check(HeadFacing({0,-1,0},f,r) && r.x<-.99f,"negative control: old hemisphere flip reversed shoulders behind player");
    check(!HeadFacing({0,0,1},f,r),"looking vertically uses caller fallback");
    check(!HeadFacing({std::numeric_limits<float>::quiet_NaN(),0,0},f,r),"nonfinite tracking rejected");
    return bad ? 1 : 0;
}
