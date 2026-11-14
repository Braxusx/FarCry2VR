#include "common/panel_anchor.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int bad=0;
    auto check=[&](bool ok,const char* m) { if(!ok) { ++bad; std::fprintf(stderr,"FAIL: %s\n",m); } };
    auto near=[](Vec3 a,Vec3 b) { auto d=Subtract(a,b); return DotProduct(d,d)<1e-10f; };
    PanelAnchor p;
    p.Update({1,1.6f,2},{0,0,-1},1,0,3);
    check(p.valid && near(p.position,{1,1.6f,-1}) && p.yaw==0,"screen opens three metres ahead at eye height");
    p.Update({2,1.8f,4},{1,0,0},1,0,3);
    check(near(p.position,{1,1.6f,-1}) && p.yaw==0,"turning and walking do not drag the screen");
    p.Update({2,1.8f,4},{1,0,0},1,2,3);
    check(near(p.position,{5,1.8f,4}) && std::fabs(p.yaw+1.5707963f)<1e-6f,"opening pause menu chooses current facing");
    p.Update({0,0,0},{0,0,-1},2,2,3);
    check(near(p.position,{0,0,-3}),"reference-space replacement reanchors");
    p.Reset(); p.Update({3,0,0},{0,0,-1},2,2,3);
    check(near(p.position,{3,0,-3}),"explicit recenter reanchors same mode");
    p.Reset(); p.Update({0,1,0},{0,1,0},2,2,3);
    check(near(p.position,{0,1,-3}) && std::isfinite(p.yaw),"vertical gaze has finite upright fallback");
    p.Reset(); p.Update({std::numeric_limits<float>::quiet_NaN(),0,0},{0,0,-1},2,2,3);
    check(!p.valid,"invalid tracking cannot establish an anchor");
    check(std::fabs(PanelHeight(2.4f,1176.0f/664.0f,3090.0f/3600.0f)-2.4f*664/1176)<1e-6f,
        "captured widescreen layout survives a portrait eye texture");
    check(std::fabs(PanelHeight(2.4f,0,3090.0f/3600.0f)-2.4f*3600/3090)<1e-6f,
        "legacy head-attached layer retains its texture aspect");
    return bad ? 1 : 0;
}
