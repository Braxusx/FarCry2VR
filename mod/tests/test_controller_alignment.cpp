#include "common/controller_alignment.h"
#include <cstdio>
using namespace fc2vr::controllers;
int main() {
    int failures=0;
    auto check=[&](bool pass,const char* message) { if(!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); } };
    const auto same=[](const Alignment& a,const Alignment& b) {
        return a.freePalm==b.freePalm && a.weaponPalm==b.weaponPalm && a.macheteRoll==b.macheteRoll;
    };
    constexpr auto touch="/interaction_profiles/oculus/touch_controller";
    constexpr auto index="/interaction_profiles/valve/index_controller";
    const Alignment upstream{false,false,55}, accepted11{true,true,85};
    const QuestPreset preset{};
    Detection d;
    check(!d.QuestTouch(0) && !d.QuestTouch(1),"startup has no preset before hardware evidence");
    d.Profile(1,touch,true);
    check(!d.QuestTouch(1),"Touch alone never classifies a headset as Quest");
    d.System("Meta Quest 3",true);
    check(!d.QuestTouch(1),"new system clears the old controller identity");
    d.Profile(0,touch,true); d.Profile(1,touch,true);
    check(d.QuestTouch(0) && d.QuestTouch(1),"captured Quest 3 + VirtualDesktopXR Touch pair");
    check(same(Resolve(upstream,preset,d.QuestTouch(1)),accepted11),"Quest reproduces accepted candidate11 alignment exactly");
    for(int i=0;i<1000;++i) {
        const auto applied=Resolve(upstream,preset,d.QuestTouch(1));
        check(same(applied,accepted11),"frame evaluation does not accumulate the offset");
    }
    check(upstream.macheteRoll==55 && !upstream.freePalm && !upstream.weaponPalm,"base calibration remains available for saving");
    d.Profile(0,index,true);
    check(!d.QuestTouch(0) && d.QuestTouch(1),"mixed controllers are resolved per hand");
    check(same(Resolve(upstream,preset,d.QuestTouch(0)),upstream),"Quest headset does not apply preset to Index hand");
    d.Profile(1,touch,false);
    check(!d.QuestTouch(1),"query failure revokes a previously recognized controller");
    d.Profile(1,touch,true); d.ClearProfiles();
    check(!d.QuestTouch(1),"profile-change event clears old hardware before refreshed poses");
    d.Profile(1,touch,true);
    check(d.QuestTouch(1),"controller reconnect selects the preset again");
    d.Profile(1,"",true);
    check(!d.QuestTouch(1),"null/absent profile uses base");
    for(auto name:{"Oculus Rift S","Oculus Rift CV1","Valve Index","HTC Vive","PICO 4","SteamVR","Oculus","Meta","Request 3","Meta Quest 30","Meta Quest 3 unknown",""}) {
        d.System(name,true); d.Profile(0,touch,true); d.Profile(1,touch,true);
        check(!d.QuestTouch(0) && !d.QuestTouch(1),"non-Quest or ambiguous system retains original calibration even with Touch bindings");
        check(same(Resolve(upstream,preset,d.QuestTouch(1)),upstream),"non-Quest base is numerically unchanged");
        const Alignment custom{true,false,-17.5f};
        check(same(Resolve(custom,preset,d.QuestTouch(1)),custom),"non-Quest custom base is preserved too");
    }
    for(auto name:{"Oculus Quest","Oculus Quest 2","Oculus Quest2","Meta Quest Pro","Meta Quest 3","Meta Quest 3S","meta quest 3"}) {
        d.System(name,true); d.Profile(1,touch,true);
        check(d.QuestTouch(1),"explicit Quest name recognized case-insensitively");
    }
    d.System("Meta Quest 3",false); d.Profile(1,touch,true);
    check(!d.QuestTouch(1),"failed system query cannot enable preset from stale name");
    d.System("",false);
    check(!d.QuestHeadset() && !d.QuestTouch(1),"teardown clears headset and controller classification");
    auto disabled=preset; disabled.automatic=false;
    check(same(Resolve(upstream,disabled,true),upstream),"opt out restores base on Quest");
    auto custom=preset; custom.freePalm=false; custom.macheteRollOffset=12.5f;
    check(same(Resolve(upstream,custom,true),{false,true,67.5f}),"Quest-only settings can be adjusted independently");
    check(!d.QuestTouch(-1) && !d.QuestTouch(2),"invalid side cannot index the array");
    std::printf("Controller alignment: %s\n",failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
