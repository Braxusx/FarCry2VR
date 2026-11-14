#pragma once
#include "pose_frame.h"
#include "pose_authority.h"

namespace fc2vr::controllers {
using fc2vr::tracking::Vec3;
// XR_EXT_palm_pose defines a static grip-to-palm transform for rigid controllers.
// Retain only this calibration through temporary tracking loss, never a pose or
// an input-authority lease. The adapter resets it on profile/context changes.
class PalmCalibration {
    Vec3 offset_{};
    bool valid_=false;
public:
    void Reset() { offset_={};valid_=false; }
    bool Observe(Vec3 gripLocal,bool trusted) {
        const float squared=fc2vr::tracking::DotProduct(gripLocal,gripLocal);
        if(!trusted || !std::isfinite(squared) || squared>0.15f*0.15f) return false;
        offset_=gripLocal;valid_=true;return true;
    }
    bool Valid() const { return valid_; }
    Vec3 Offset() const { return offset_; }
};
// Apply after grip filtering, in controller-local coordinates and metres. The
// existing tracking/reference mapping performs world scaling exactly once.
// This does not alter the runtime grip, aim direction, or tracking evidence.
template<class Pose> bool PalmPosition(const Pose& grip,Vec3 offset,Vec3& out) {
    if(!fc2vr::authority::FinitePosition(grip)||!fc2vr::authority::FiniteOrientation(grip)) return false;
    PalmCalibration checked;if(!checked.Observe(offset,true)) return false;
    const auto& q=grip.orientation;
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    const float inverse=1/std::sqrt(norm);
    const Vec3 axis{q.x*inverse,q.y*inverse,q.z*inverse};const float w=q.w*inverse;
    const auto cross=[](Vec3 a,Vec3 b) { return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; };
    const auto t=fc2vr::tracking::Scale(cross(axis,offset),2);
    const auto rotated=fc2vr::tracking::Add(offset,fc2vr::tracking::Add(fc2vr::tracking::Scale(t,w),cross(axis,t)));
    out=fc2vr::tracking::Add({grip.position.x,grip.position.y,grip.position.z},rotated);return true;
}
} // namespace fc2vr::controllers
