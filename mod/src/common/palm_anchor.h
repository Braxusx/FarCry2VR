#pragma once
#include "pose_frame.h"
namespace fc2vr::tracking {
// A controller grip represents the palm, while FC2's Hand joint is the wrist.
// Estimate the palm halfway to the four long-finger knuckles. Use hierarchy
// children supplied by the adapter, never guessed bone indices or fingertips.
inline bool PalmOffset(const Vec3* children,unsigned count,Vec3& out) {
    if (!children || count<4 || count>16) return false;
    for (unsigned seed=0;seed<count;++seed) {
        const float a=DotProduct(children[seed],children[seed]);
        if (!std::isfinite(a) || a<.055f*.055f || a>.13f*.13f) continue;
        Vec3 sum{}; unsigned n=0;
        for (unsigned i=0;i<count;++i) {
            const float b=DotProduct(children[i],children[i]);
            if (!std::isfinite(b) || b<.055f*.055f || b>.13f*.13f) continue;
            if (DotProduct(children[seed],children[i])<.70f*std::sqrt(a*b)) continue;
            sum=Add(sum,children[i]); ++n;
        }
        if (n==4) { out=Scale(sum,.125f); return true; }
    }
    return false;
}
} // namespace fc2vr::tracking
