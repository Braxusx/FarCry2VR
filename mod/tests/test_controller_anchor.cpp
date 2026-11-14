#include "common/controller_anchor.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
using namespace fc2vr::controllers;
struct Pose { struct {float x,y,z,w;} orientation{0,0,0,1};Vec3 position{}; };
int main() {
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* label) { ++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",label);} };
    const auto near=[](Vec3 a,Vec3 b) { const auto d=Subtract(a,b);return DotProduct(d,d)<1e-10f; };
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    PalmCalibration cache;
    check(!cache.Valid(),"missing runtime extension/data has no calibration");
    check(!cache.Observe({0,0,.04f},false) && !cache.Valid(),"inactive/untracked action cannot seed calibration");
    check(cache.Observe({.01f,-.02f,.04f},true) && cache.Valid(),"runtime offset becomes available from trusted observation");
    const auto offset=cache.Offset();
    for(Vec3 invalid: {Vec3{nan,0,0},Vec3{0,inf,0},Vec3{0,0,.151f}}) {
        check(!cache.Observe(invalid,true),"nonfinite or implausible runtime offset rejected");
        check(near(cache.Offset(),offset),"bad sample cannot replace known controller calibration");
    }
    check(!cache.Observe({},false) && cache.Valid(),"temporary pose loss retains static calibration without supplying tracking authority");
    cache.Reset();check(!cache.Valid(),"profile/context reset cannot reuse old hardware calibration");
    check(cache.Observe({},true),"a runtime can legitimately report coincident grip/palm origins");
    Pose grip;grip.position={.3f,1.2f,-.4f};Vec3 palm{};
    check(PalmPosition(grip,offset,palm) && near(palm,{.31f,1.18f,-.36f}),"offset is relative to controller grip in metres");
    const auto original=grip;
    for(int frame=0;frame<100;++frame) {
        Vec3 current{};check(PalmPosition(grip,offset,current) && near(current,palm),"static correction does not accumulate each frame");
    }
    check(near(grip.position,original.position) && grip.orientation.w==original.orientation.w,"runtime pose is never changed by calibration");
    // Controller-local +Z maps to +X after +90 degrees about local Y. Use
    // independently known axis rotations to catch applying the offset in world axes.
    const float q=.7071067811865475f;
    grip.orientation={0,q,0,q};
    check(PalmPosition(grip,offset,palm) && near(palm,{.34f,1.18f,-.41f}),"palm offset rotates with controller yaw");
    grip.orientation={q,0,0,q};
    check(PalmPosition(grip,offset,palm) && near(palm,{.31f,1.16f,-.42f}),"palm offset rotates with controller pitch");
    grip.orientation={0,0,q,q};
    check(PalmPosition(grip,offset,palm) && near(palm,{.32f,1.21f,-.36f}),"palm offset rotates with controller roll");
    grip.orientation={0,0,-q,-q};Vec3 negative{};
    check(PalmPosition(grip,offset,negative) && near(negative,palm),"quaternion sign cannot change alignment");
    grip.orientation={0,0,q*1.2f,q*1.2f};
    check(PalmPosition(grip,offset,negative) && near(negative,palm),"valid nonunit quaternion normalizes without scaling the offset");
    ReferenceFrame reference;reference.origin={.2f,.5f,-.1f};reference.right={0,0,-1};reference.forward={-1,0,0};
    GameBasis game;game.P={20,30,2};
    for(float worldScale:{.7f,1.0f,1.6f}) {
        const auto rawWorld=game.Position(Scale(reference.Position(grip.position),worldScale));
        const auto palmWorld=game.Position(Scale(reference.Position(palm),worldScale));
        const auto delta=Subtract(palmWorld,rawWorld);
        check(std::fabs(std::sqrt(DotProduct(delta,delta))-std::sqrt(DotProduct(offset,offset))*worldScale)<3e-6f,"recenter preserves offset and world scale applies exactly once");
    }
    Vec3 unchanged{7,8,9};
    grip.orientation={0,0,0,0};check(!PalmPosition(grip,offset,unchanged) && near(unchanged,{7,8,9}),"invalid grip orientation keeps output untouched");
    grip.orientation={0,0,0,1};grip.position.x=nan;
    check(!PalmPosition(grip,offset,unchanged),"invalid grip position cannot create a valid aligned point");
    grip.position={};check(!PalmPosition(grip,{0,0,1},unchanged),"oversized correction is not applied");
    std::printf("controller anchor: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
