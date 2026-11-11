#include "common/room_scale.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* text){if(!ok){++failures;std::printf("FAIL %s\n",text);}};
    auto near=[](Vec3 a,Vec3 b){auto d=Subtract(a,b);return DotProduct(d,d)<1e-10f;};
    RoomScaleLedger ledger;GameBasis basis;
    const auto epoch=ledger.Epoch();
    ledger.Request({.3f,0,0},basis,{},epoch,true);
    const auto firstBasis=ledger.RequestBasis();
    ledger.Commit({.04f,0,0},firstBasis,epoch);
    check(near(ledger.Remaining(),{.26f,0,0}),"second simulation update consumes remaining offset only");
    check(near(ledger.Take(),{.04f,0,0})&&near(ledger.Take(),{}),"each body move acknowledged exactly once");
    // New render still displays the prior scene; acknowledgement has a delay.
    ledger.Request({.3f,0,0},basis,{},epoch,true);
    check(near(ledger.Remaining(),{.26f,0,0}),"unacknowledged movement survives repeated render requests");
    // Turn between request and completion. Completion uses its saved basis.
    GameBasis turn;turn.R={0,1,0};turn.F={-1,0,0};
    ledger.Request({0,.26f,0},turn,{.04f,0,0},epoch,true);
    ledger.Commit({.03f,0,0},firstBasis,epoch);
    check(near(ledger.Take(),{.03f,0,0}),"late move retains source tracking direction across turn");
    check(near(ledger.Remaining(),{0,.23f,0}),"pending body follow rotates with requested tracking basis");
    ledger.Request({0,.23f,0},turn,{.07f,0,0},epoch,true);
    check(near(ledger.Remaining(),{0,.23f,0}),"acknowledged movement is never subtracted twice");
    ledger.Reset();ledger.Commit({.1f,0,0},basis,epoch);
    ledger.Request({1,2,0},basis,{},epoch,true);
    check(near(ledger.Total(),{})&&near(ledger.Remaining(),{}),"recenter rejects old commit and request");
    ledger.Request({1,2,0},basis,{},ledger.Epoch(),false);
    check(near(ledger.Remaining(),{}),"inactive tracking cannot move the body");
    auto close=[](float a,float b){return std::fabs(a-b)<1e-6f;};
    check(close(RoomScaleLeash({0,-.08f,0},{0,1,0},.10f),.02f),"backstep starts following before eyes enter torso");
    check(close(RoomScaleLeash({0,.08f,0},{0,1,0},.10f),.10f),"forward lean keeps existing allowance");
    check(close(RoomScaleLeash({.08f,0,0},{0,1,0},.10f),.10f),"side lean keeps existing allowance");
    check(close(RoomScaleLeash({0,-.08f,0},{0,1,0},.01f),.01f),"never enlarge a smaller configured leash");
    check(close(RoomScaleLeash({0,-.08f,0},{0,1,0},.10f,.10f),.10f),"matching rear allowance restores legacy radial leash");
    check(close(RoomScaleLeash({0,-.08f,0},{0,1,0},.10f,0),0),"zero rear allowance remains configurable");
    check(close(RoomScaleLeash({0,-.08f,0},{0,0,1},.10f),.10f),"vertical view cannot infer rear direction");
    check(close(RoomScaleLeash({0,-.08f,0},{0,std::numeric_limits<float>::quiet_NaN(),0},.10f),.10f),"invalid direction falls back");
    check(close(RoomScaleLeash({0,0,0},{0,1,0},.10f),.10f),"zero offset is stable");
    // Simulate the existing radial follow step for every direction and yaw. A
    // speed- or wall-limited caller may move less, but the helper never asks to
    // move beyond the head, changes height, or adds a sideways correction.
    for(int yaw=0;yaw<360;yaw+=15) for(int direction=0;direction<360;direction+=5) {
        const float y=yaw*.01745329252f,d=direction*.01745329252f;
        const Vec3 forward{std::sin(y),std::cos(y),0};
        const Vec3 offset{.16f*std::sin(d),.16f*std::cos(d),0};
        const float leash=RoomScaleLeash(offset,forward,.10f);
        check(leash>=0 && leash<=.100001f,"directional allowance stays bounded");
        const Vec3 remaining=Scale(offset,leash/.16f);
        check(-DotProduct(remaining,forward)<=.020001f,"rear residual is at most two centimetres at any yaw");
        if(DotProduct(offset,forward)>=0) check(close(leash,.10f),"head yaw preserves forward hemisphere leaning");
        const Vec3 rotatedOffset{offset.y,-offset.x,0},rotatedForward{forward.y,-forward.x,0};
        check(close(leash,RoomScaleLeash(rotatedOffset,rotatedForward,.10f)),"stick/world yaw does not change backstep allowance");
    }
    // The unchanged ledger accounts for partial native moves exactly once,
    // including a new request while the render acknowledgement is delayed.
    ledger.Reset();const auto backEpoch=ledger.Epoch();
    ledger.Request({0,-.08f,0},basis,{},backEpoch,true);
    const float rearLeash=RoomScaleLeash(ledger.Remaining(),basis.F,.10f);
    ledger.Commit({0,-.03f,0},basis,backEpoch); // native speed/collision cap
    ledger.Request({0,-.08f,0},basis,{},backEpoch,true);
    check(near(ledger.Remaining(),{0,-.05f,0}),"partial backstep is not requested twice");
    ledger.Commit({0,-(.05f-rearLeash),0},basis,backEpoch);
    check(near(ledger.Remaining(),{0,-.02f,0}),"two partial steps stop at rear allowance");
    std::printf("room scale: %d failures\n",failures);return failures?1:0;
}
