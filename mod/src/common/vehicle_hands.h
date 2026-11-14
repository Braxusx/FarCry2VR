#pragma once
#include "pose_frame.h"

namespace fc2vr::tracking {
// Move the visual tracking origin between two updates of the same seat. The
// origin follows the full seat transform; its axes follow the current native
// Camera-bone yaw, like the eye adapter. Pitch/roll never tilt the tracking axes.
// Both roots and game.P must use the same (preferably nearby) coordinate origin.
inline bool FollowVehicleSeat(GameBasis& game, const float* from, const float* to, Vec3 cameraForward) {
    for (unsigned i=0;i<16;++i) if (!std::isfinite(from[i]) || !std::isfinite(to[i])) return false;
    auto finite=[](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(game.P) || !finite(game.F) || !finite(cameraForward)) return false;
    const Vec3 movement{to[12]-from[12],to[13]-from[13],to[14]-from[14]};
    if (DotProduct(movement,movement)>=2.25f) return false;
    // Invert the source's 3x3 in double precision. Roots may contain scale;
    // a transpose alone would distort the seat-relative camera offset.
    const double ax=from[0],ay=from[1],az=from[2], bx=from[4],by=from[5],bz=from[6], cx=from[8],cy=from[9],cz=from[10];
    const double dx=static_cast<double>(game.P.x)-from[12],dy=static_cast<double>(game.P.y)-from[13],dz=static_cast<double>(game.P.z)-from[14];
    const double det=ax*(by*cz-bz*cy)+ay*(bz*cx-bx*cz)+az*(bx*cy-by*cx);
    if (!std::isfinite(det) || std::fabs(det)<1e-8) return false;
    const double x=(dx*(by*cz-bz*cy)+dy*(bz*cx-bx*cz)+dz*(bx*cy-by*cx))/det;
    const double y=(dx*(cy*az-cz*ay)+dy*(cz*ax-cx*az)+dz*(cx*ay-cy*ax))/det;
    const double z=(dx*(ay*bz-az*by)+dy*(az*bx-ax*bz)+dz*(ax*by-ay*bx))/det;
    if (!std::isfinite(x+y+z) || x*x+y*y+z*z>9) return false;
    const Vec3 next{
        static_cast<float>(to[12]+x*to[0]+y*to[4]+z*to[8]),
        static_cast<float>(to[13]+x*to[1]+y*to[5]+z*to[9]),
        static_cast<float>(to[14]+x*to[2]+y*to[6]+z*to[10])};
    const Vec3 change=Subtract(next,game.P);
    if (!finite(next) || DotProduct(change,change)>=2.25f) return false;
    const float n2=cameraForward.x*cameraForward.x+cameraForward.y*cameraForward.y;
    if (!std::isfinite(n2) || n2<1e-8f) return false;
    const float inv=1/std::sqrt(n2);
    const Vec3 forward{cameraForward.x*inv,cameraForward.y*inv,0};
    if (DotProduct(forward,game.F)<=0) return false; // discontinuity: preserve legacy fallback
    game.P=next; game.F=forward; game.R={forward.y,-forward.x,0}; game.U={0,0,1};
    return true;
}
} // namespace fc2vr::tracking
