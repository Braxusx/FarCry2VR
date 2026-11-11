#include "common/body_transport.h"
#include <cstdio>
#include <limits>
using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* text){if(!ok){++failures;std::printf("FAIL %s\n",text);}};
    auto near=[](Vec3 a,Vec3 b){auto d=Subtract(a,b);return DotProduct(d,d)<1e-8f;};
    TurnIntent turnIntent;
    check(turnIntent.Update(1000,true,true),"stick turning bypasses sway immediately");
    check(turnIntent.Update(1022,false,true),"queued native turn remains deliberate after stick release");
    check(!turnIntent.Update(1160,false,true),"sprint sway policy resumes after turn settles");
    turnIntent.Update(1200,true,true);
    check(!turnIntent.Update(1210,false,false),"mode exit clears turn intent");
    check(!turnIntent.Update(1220,false,true),"mode reentry cannot inherit an old turn");
    float from[16]={1,0,0,0,0,1,0,0,0,0,1,0,10,20,0,1};
    float to[16]={0,1,0,0,-1,0,0,0,0,0,1,0,10.1f,20.2f,0,1};
    GameBasis game;game.P={10.02f,20.12f,1.7f};
    check(FollowOnFootBody(game,from,to),"on-foot turn accepted");
    check(near(game.P,{9.98f,20.22f,1.7f}),"stable camera offset rotates around moving root");
    check(near(game.F,{-1,0,0}) && near(game.R,{0,1,0}),"hands follow deliberate turn with body");
    check(near(game.Position({.3f,-.4f,-.5f}),{9.48f,20.52f,1.3f}),"controller follows matching eye basis");
    // Physical step is already in tracking-space input. Body follow must not
    // add it a second time; test both untranslated and rotated game frames.
    for(int turn=0;turn<2;++turn) {
        float target[16];for(int i=0;i<16;++i)target[i]=turn?to[i]:from[i];
        target[12]=10.1f;target[13]=20.2f;
        GameBasis g;g.P={10,20,1.7f};
        const Vec3 room=turn?Vec3{.2f,0,.1f}:Vec3{.1f,0,-.2f};
        check(FollowOnFootBody(g,from,target,room),"physical body follow accepted");
        check(near(g.P,{10,20,1.7f}),"room follow removed once while native root remains moved");
    }
    // Native jump/crouch height follows immediately; no Camera-bone bob input.
    to[14]=.3f;game={};game.P={10,20,1.7f};
    check(FollowOnFootBody(game,from,to)&&near(game.P,{10.1f,20.2f,2}),"native vertical movement retained");
    const auto before=game;to[4]=std::numeric_limits<float>::quiet_NaN();
    check(!FollowOnFootBody(game,from,to)&&near(game.P,before.P),"invalid transform leaves fallback intact");
    std::printf("body transport: %d failures\n",failures);return failures?1:0;
}
