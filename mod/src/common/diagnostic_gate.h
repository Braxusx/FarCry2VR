#pragma once
#include <cstdint>
#include <cmath>
namespace fc2vr::diagnostics {
// Observation only. A transient inventory pointer during loading is not a
// playable weapon. Require the same identified, attached model continuously.
struct ArmedGate {
    std::uintptr_t owner=0;
    std::uint32_t key=0;
    double since=0,last=0;
    double Update(double now,bool playable,std::uintptr_t weapon,std::uint32_t modelKey) {
        if (!std::isfinite(now) || !playable || !weapon || !modelKey) {
            owner=0; key=0; since=last=0; return 0;
        }
        if (weapon!=owner || modelKey!=key || now<last || now-last>250) {
            owner=weapon; key=modelKey; since=now;
        }
        last=now; return now-since;
    }
};
}
