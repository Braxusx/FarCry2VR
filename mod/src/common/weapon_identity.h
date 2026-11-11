#pragma once
#include <cstdint>

namespace fc2vr::weapons {
enum class Kind { None, Unknown, Other, Melee };
struct Identity {
    uintptr_t owner = 0;
    Kind kind = Kind::None;
    bool MeleeFor(uintptr_t weapon) const { return weapon && owner == weapon && kind == Kind::Melee; }
};

// Read only the currently equipped native weapon. A requested inventory slot,
// HUD reticle or model bone count cannot establish which weapon was equipped.
// This is the same strategy chain used by the native melee-hit integration.
template<class ReadPointer>
Identity Inspect(uintptr_t weapon, uintptr_t meleeVtable, ReadPointer read) {
    if (!weapon) return {};
    Identity result{weapon, Kind::Unknown};
    uintptr_t controller=0, strategy=0, table=0;
    if (!meleeVtable || !read(weapon+0x24,controller) || !controller
        || !read(controller+0x54,strategy) || !strategy || !read(strategy,table) || !table) return result;
    result.kind = table == meleeVtable ? Kind::Melee : Kind::Other;
    return result;
}
}
