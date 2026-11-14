#include "common/vehicle_hands.h"
#include <cstdio>
#include <cstring>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool pass,const char* name) { if(!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",name); } };
    auto near=[](Vec3 a,Vec3 b,float tolerance=0.00002f) { const auto d=Subtract(a,b); return DotProduct(d,d)<tolerance*tolerance; };
    float from[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    float to[16]; std::memcpy(to,from,sizeof from); to[12]=.1f; to[13]=.2f;
    GameBasis g; g.P={.2f,.4f,1.4f};
    check(FollowVehicleSeat(g,from,to,{0,1,0}) && near(g.P,{.3f,.6f,1.4f}),"pure translation preserves existing behavior");
    // Rotated, scaled source: transporting its camera offset must invert scale.
    from[0]=from[5]=from[10]=2;
    const float s=.5f,c=.866025404f;
    to[0]=c; to[1]=s; to[4]=-s; to[5]=c;
    g.P={.4f,.8f,2.8f};
    check(FollowVehicleSeat(g,from,to,{-s,c,.2f}),"scaled seat and tilted camera accepted");
    check(near(g.P,{.1f+.2f*c-.4f*s,.2f+.2f*s+.4f*c,1.4f}),"camera offset follows rotation as well as translation");
    check(near(g.U,{0,0,1}) && g.F.z==0,"tracking basis stays upright on banked seat");
    auto unchanged=[&](const char* name,const float* a,const float* b,Vec3 f) {
        GameBasis before=g; const bool ok=FollowVehicleSeat(g,a,b,f);
        check(!ok && std::memcmp(&before,&g,sizeof g)==0,name);
    };
    unchanged("zero horizontal forward cannot corrupt frame",from,to,{0,0,1});
    unchanged("opposite yaw is a discontinuity",from,to,{s,-c,0});
    float invalid[16]; std::memcpy(invalid,from,sizeof from); invalid[0]=0;
    unchanged("singular source rejected",invalid,to,{0,1,0});
    std::memcpy(invalid,to,sizeof to); invalid[12]=5;
    unchanged("teleport rejected",from,invalid,{0,1,0});
    std::memcpy(invalid,to,sizeof to); invalid[0]=std::numeric_limits<float>::quiet_NaN();
    unchanged("nonfinite target rejected",from,invalid,{0,1,0});
    std::printf("vehicle hand transport tests: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
