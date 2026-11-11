#include "common/action_context.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static void Check(const char* name,bool use,bool move) {
    auto context=fc2vr::input::OnFootContext(name);
    if (context.interaction!=use || context.locomotion!=move) {
        std::fprintf(stderr,"wrong native action permissions: %s\n",name?name:"null");
        std::exit(1);
    }
}
int main() {
    // Native XML imports: briefing allows use and move without weapon actions;
    // briefinginteraction allows use but deliberately omits locomotion.
    Check("default",true,true); Check("gameplay",true,true);
    Check("briefing",true,true); Check("briefing_no_map",true,true);
    Check("map_briefing",true,true); Check("briefinginteraction",true,false);
    // Reading a map on foot retains movement without enabling VR interaction
    // targeting. Opening a map in a vehicle must not move the character capsule.
    Check("mapcompass",false,true); Check("map_driving",false,false);
    Check("loading",false,false); Check("cinematic",false,false);
    Check("vehicle",false,false); Check("turret",false,false);
    for (const char* name: {"driving","passenger","in_vehicle","ied_in_vehicle",
        "vehicle_transitions","using_mounted_weapon","paragliderdriving",
        "watch","healPlayer","lyingdown","ladder","machete_finish","pausemenu","menus"})
        Check(name,false,false);
    Check("pause",false,false); Check("unknown",false,false); Check(nullptr,false,false);
    std::puts("scripted interaction and locomotion permissions passed");
}
