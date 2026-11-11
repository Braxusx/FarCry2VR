#include "common/ride_detector.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool pass,const char* message) { if(!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); } };
    RideDetector d;
    auto step=[&](double t,float x,bool stick=false,bool vehicle=false,bool blocked=false,float height=1.0f,std::uintptr_t owner=1) {
        return d.Update(t,{x,0,0},owner,2,true,stick,vehicle,blocked,height);
    };
    check(!step(0,0),"stationary start has no ride evidence");
    check(!step(11,.001f),"one millimetre does not trigger");
    check(step(22,.005f),"slow coherent start engages in 22ms, below the old 2.5m/s threshold");
    for (int i=3;i<900;++i) check(step(i*11,.005f),"traffic stop retains the same seat for ten seconds");
    check(step(9900,.010f),"departure from stop needs no second warmup");
    check(step(11000,.012f),"long render hitch retains active ride");
    check(!step(11011,.013f,true),"deliberate locomotion ends passive inference");
    check(!step(11100,.1f),"release coasting cannot immediately relatch");
    check(!step(11450,.1f),"post-input observation window restarts");
    check(step(11461,.106f),"new passive movement can qualify");
    check(!step(11472,.106f,false,false,false,1.3f),"standing out of the seat ends ride");
    check(step(11500,.106f,true,true),"native vehicle authority takes priority over stick");
    check(!step(11511,.106f,false,false,false,1.0f,7),"owner replacement drops old seat");
    check(step(11522,.112f,false,false,false,1.0f,7),"new owner needs its own movement evidence");
    check(!step(11533,.112f,false,false,true,1.0f,7),"menu or loading reset");
    check(!step(11544,.112f,false,false,false,1.0f,7),"no old seat after menu");
    check(!step(11555,100,false,false,false,1.0f,7),"teleport cannot latch");
    check(!step(11566,100.1f,false,false,false,1.0f,7),"teleport quarantine");
    d={};
    for(int i=0;i<400;++i) check(!step(i*11,(i%2)*.001f),"bounded idle noise does not accumulate path length");
    check(!d.Update(4500,{0,0,0},0,2,true,false,false,false,1),"missing skeleton rejected");
    check(!d.Update(4511,{0,0,0},1,2,false,false,true,false,1),"invalid body rejected even in vehicle");
    check(!d.Update(4522,{std::numeric_limits<float>::quiet_NaN(),0,0},1,2,true,false,false,false,1),"nonfinite position rejected");
    check(!d.Update(4533,{0,0,0},1,2,true,false,false,false,std::numeric_limits<float>::infinity()),"nonfinite posture rejected");
    std::printf("Ride transitions: %s\n",failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
