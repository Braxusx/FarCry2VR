#pragma once
#include "tracking_frame.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace fc2vr::tracking {
struct NativeViewSample {
    std::array<float,12> angles{}; // desired look/aim, effective look/aim; observation only
    std::uint64_t shotSequence=0;
    std::uintptr_t pawnData=0;
    std::uintptr_t weapon=0;
    std::uint32_t desiredStance=0,effectiveStance=0;
    double timeMs=0,shotTimeMs=0;
    bool valid=false;
};
struct BodyPose {
    Vec3 camera{};
    std::array<float,16> root{};
    std::uint64_t sequence = 0;
    std::uintptr_t skeleton = 0, graphics = 0;
    double timeMs = 0;
    bool valid = false;
    NativeViewSample nativeView{};
};
struct BodyPoses {
    BodyPose latest{}, previous{};
    std::array<BodyPose,6> older{};
    const BodyPose& Select(bool previousUpdate) const { return previousUpdate ? previous : latest; }
    // The render camera identifies the queued scene update. Worker scheduling
    // determines whether that update is latest or previous when we read it.
    // Only accept a near-exact Camera-bone match; never extrapolate the seat.
    BodyPose MatchCamera(Vec3 camera, double nowMs) const {
        auto distance2=[](Vec3 a,Vec3 b) { const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z; return x*x+y*y+z*z; };
        BodyPose best{};
        float bestError=1e-6f; // 1 mm; captured native/bone disagreement was <= 0.002 mm
        const BodyPose* candidates[8]={&latest,&previous,&older[0],&older[1],&older[2],&older[3],&older[4],&older[5]};
        for (const auto* p:candidates) {
            if (!p->valid || !std::isfinite(nowMs) || !std::isfinite(p->timeMs)
                || nowMs<p->timeMs || nowMs-p->timeMs>250) continue;
            const float error=distance2(camera,p->camera);
            if (!std::isfinite(error) || error>bestError) continue;
            bool finite=true; for (float v:p->root) finite=finite && std::isfinite(v);
            if (!finite) continue;
            // Equal camera positions can describe multiple updates at rest.
            // Retain the newest on ties; reject if their seat transforms differ.
            if (best.valid && std::fabs(error-bestError)<1e-10f) continue;
            best=*p; bestError=error;
        }
        if (best.valid) for (const auto* p:candidates) {
            if (!p->valid || !std::isfinite(p->timeMs) || nowMs<p->timeMs || nowMs-p->timeMs>250) continue;
            const float error=distance2(camera,p->camera);
            if (!std::isfinite(error) || std::fabs(error-bestError)>=1e-10f) continue;
            for (unsigned i=0;i<16;++i)
                if (!std::isfinite(p->root[i]) || std::fabs(p->root[i]-best.root[i])>0.001f) return {};
        }
        return best;
    }
};
// The native adapter holds its lock for both Publish and Capture. Returning a
// value lets render consumers retain the exact sample across worker updates.
class BodyPoseHistory {
    BodyPoses poses{};
    std::uint64_t sequence = 0;
public:
    void Publish(Vec3 camera, const float* root, std::uintptr_t skeleton,
                 std::uintptr_t graphics, double timeMs, const NativeViewSample& nativeView = {}) {
        const bool sameOwner = poses.latest.valid && poses.latest.skeleton == skeleton && poses.latest.graphics == graphics;
        if (sameOwner) {
            for (unsigned i=5;i>0;--i) poses.older[i]=poses.older[i-1];
            poses.older[0]=poses.previous;
            poses.previous=poses.latest;
        } else poses={};
        BodyPose next;
        next.camera = camera;
        for (unsigned i=0; i<16; ++i) next.root[i] = root[i];
        next.sequence = ++sequence; next.skeleton = skeleton; next.graphics = graphics;
        next.timeMs = timeMs; next.valid = true;
        next.nativeView=nativeView;
        poses.latest = next;
    }
    BodyPoses Capture() const { return poses; }
};
struct BodyAnchor {
    Vec3 position{};
    std::uint64_t sequence = 0;
    bool valid = false, root = false;
    std::array<float,16> transform{};
    std::uintptr_t skeleton = 0, graphics = 0;
    bool followSeat = false; // full vehicle transform; on-foot roots use upright yaw
    NativeViewSample nativeView{};
    double timeMs=0;
};
inline BodyAnchor AnchorFor(const BodyPose& pose, bool root, bool followSeat = false) {
    return {root ? Vec3{pose.root[12],pose.root[13],pose.root[14]} : pose.camera,
            pose.sequence,pose.valid,root,pose.root,pose.skeleton,pose.graphics,followSeat,pose.nativeView,pose.timeMs};
}
} // namespace fc2vr::tracking
