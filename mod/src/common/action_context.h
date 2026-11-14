#pragma once
#include <cstring>

namespace fc2vr::input {
struct Context { bool interaction=false, locomotion=false; };
// Verified against Config\InputActionMapSingle.xml and Common.xml (GOG 1.03).
// Briefing imports use/look/move, but not weapons. Keep this distinct from
// NormalControls: treating all scripted maps as ordinary gameplay enables fire,
// weapon holsters and other actions during sequences which prohibit them.
inline Context OnFootContext(const char* name) {
    if (!name) return {};
    if (!std::strcmp(name,"default") || !std::strcmp(name,"gameplay")
        || !std::strcmp(name,"briefing") || !std::strcmp(name,"briefing_no_map")
        || !std::strcmp(name,"map_briefing")) return {true,true};
    if (!std::strcmp(name,"briefinginteraction")) return {true,false};
    // mapcompass imports default -> gameplay -> move in the native XML. The
    // carried map must not suspend physical locomotion. Keep VR interaction
    // targeting separate so the map's trigger/monocular bindings stay native.
    if (!std::strcmp(name,"mapcompass")) return {false,true};
    return {};
}
}
