#pragma once
#include <cstdint>
#include <limits>

namespace fc2vr::native {
// These are already GOG preferred VAs, not Steam VAs for gameaddr::Xlate.
// Only the verified GOG image admits this adapter. Per-site byte guards remain
// mandatory; this helper merely rebases within its known image extent.
inline std::uintptr_t GogImageAddress(std::uintptr_t base,bool gog,std::uintptr_t va) {
    constexpr std::uintptr_t preferred=0x10000000u,size=0x0192a000u;
    if(!gog || !base || va<preferred || va-preferred>=size
        || base>(std::numeric_limits<std::uintptr_t>::max)()-(size-1)) return 0;
    return base+(va-preferred);
}
}
