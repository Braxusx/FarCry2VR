#include "common/startup_policy.h"
#include <cstdlib>

int main() {
    using fc2vr::startup::WorldCamera;
    for(int player=0;player<2;++player)
        for(int load=0;load<2;++load)
            for(int menu=0;menu<2;++menu)
                if(WorldCamera(player,load,menu)!=(player&&!load&&!menu)) return EXIT_FAILURE;
    // Re-entry is not latched: a successful gameplay frame does not authorize a
    // later menu, loading frame, or missing player. Unarmed/vehicle status is not
    // part of this policy and is not mistaken for missing world ownership.
    if(!WorldCamera(true,false,false) || WorldCamera(false,false,false)
        || WorldCamera(true,true,false) || WorldCamera(true,false,true)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
