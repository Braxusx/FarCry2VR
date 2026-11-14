#pragma once
#include "pose_frame.h"
namespace fc2vr::tracking {
// Dunia uses Z up. Right follows the actual head yaw even when facing backward.
inline bool HeadFacing(Vec3 forward,Vec3& f,Vec3& right) {
    const float n=std::sqrt(forward.x*forward.x+forward.y*forward.y);
    if (!std::isfinite(n) || n<.2f) return false;
    f={forward.x/n,forward.y/n,0}; right={f.y,-f.x,0}; return true;
}
inline float BodyYawDelta(Vec3 body,Vec3 head) {
    return std::atan2(body.x*head.y-body.y*head.x,body.x*head.x+body.y*head.y);
}
} // namespace fc2vr::tracking
