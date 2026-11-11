#include "common/palm_anchor.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool pass,const char* message) { if(!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); } };
    auto near=[](Vec3 a,Vec3 b) { auto d=Subtract(a,b); return DotProduct(d,d)<1e-10f; };
    // Wrist-local long-finger roots and thumb measured from candidate 4's
    // skeleton capture. Thumb is deliberately first: order is not anatomy.
    Vec3 children[]={{.0168f,.0125f,.0292f},{.0851f,-.0054f,.0382f},
        {.0881f,-.0055f,.0111f},{.0903f,-.0050f,-.0149f},{.0838f,-.0014f,-.0390f},
        {-.09f,0,0}}; // opposite forearm/helper must not become a finger
    const Vec3 expected{.0434125f,-.0021625f,-.000575f};
    Vec3 palm{};
    check(PalmOffset(children,6,palm) && near(palm,expected),"captured hand gives 43.47mm palm instead of wrist");
    check(std::sqrt(DotProduct(palm,palm))>.04f,"negative control: wrist anchor is centimetres away");
    for(auto& c:children) c.z=-c.z;
    check(PalmOffset(children,6,palm) && near(palm,{expected.x,expected.y,-expected.z}),"mirrored left hand uses its own palm");
    // Rigid rotation of the bone basis must rotate the estimate, not move it
    // along a fixed world/controller axis.
    for(auto& c:children) c={-c.y,c.x,c.z};
    check(PalmOffset(children,6,palm) && near(palm,{-expected.y,expected.x,-expected.z}),"palm estimate follows wrist orientation");
    Vec3 invalid[5]={{.08f,0,0},{.09f,0,0},{.10f,0,0},{.11f,0,0},{.12f,0,0}};
    Vec3 untouched{1,2,3};
    check(!PalmOffset(invalid,5,untouched) && near(untouched,{1,2,3}),"ambiguous five-finger cluster preserves fallback");
    check(!PalmOffset(nullptr,5,untouched),"null rejected");
    check(!PalmOffset(children,3,untouched),"missing fingers rejected");
    check(!PalmOffset(children,17,untouched),"oversized hierarchy rejected without access");
    invalid[0].x=std::numeric_limits<float>::quiet_NaN();
    check(!PalmOffset(invalid,4,untouched),"nonfinite finger does not complete cluster");
    std::printf("Palm anchor: %s\n",failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
