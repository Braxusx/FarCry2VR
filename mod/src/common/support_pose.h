#pragma once
#include "late_pose.h"

namespace fc2vr::presentation {
// Captured from the native hand solve BEFORE its two-hand rotation. These are
// controller-local game units, not a new aim/interaction or animation state.
enum class SupportMode { None, Rifle, Pistol, PistolGrip };
struct SupportHold {
    SupportMode mode=SupportMode::None;
    Vec3 pivot{},foregrip{};
};
inline Vec3 PosePoint(const Matrix& m,Vec3 p) {
    return {p.x*m[0]+p.y*m[4]+p.z*m[8]+m[12],
        p.x*m[1]+p.y*m[5]+p.z*m[9]+m[13],p.x*m[2]+p.y*m[6]+p.z*m[10]+m[14]};
}
// Existing pistol contact offset, expressed in controller axes. The caller
// supplies either the legacy wrist or the calibrated main-hand palm as origin.
inline Vec3 PistolSupportPoint(Vec3 origin,Vec3 right,Vec3 up,Vec3 back) {
    return {origin.x-right.x*.02f-up.x*.07f-back.x*.02f,
        origin.y-right.y*.02f-up.y*.07f-back.y*.02f,
        origin.z-right.z*.02f-up.z*.07f-back.z*.02f};
}
// Preserve the controller-driven wrist orientation, including saved angles,
// while placing the visible support palm at the pistol contact point.
inline bool PlaceSupportPalm(const Matrix& hand,Vec3 localPalm,Vec3 contact,Matrix& out) {
    using namespace fc2vr::tracking;
    if(!detail::Affine(hand)||!detail::Finite(localPalm)||!detail::Finite(contact)
        || DotProduct(localPalm,localPalm)>.15f*.15f) return false;
    const auto shift=Subtract(contact,PosePoint(hand,localPalm));
    auto result=hand;result[12]+=shift.x;result[13]+=shift.y;result[14]+=shift.z;
    if(!detail::Affine(result)) return false;
    out=result;return true;
}
inline bool CaptureSupport(const Matrix& right,Vec3 pivot,Vec3 foregrip,SupportMode mode,SupportHold& output) {
    Matrix inverse{};
    if(!detail::Rigid(right)||!detail::Finite(pivot)||!detail::Finite(foregrip)||!Inverse(right,inverse)) return false;
    SupportHold hold{mode,PosePoint(inverse,pivot),PosePoint(inverse,foregrip)};
    const auto d=fc2vr::tracking::Subtract(hold.foregrip,hold.pivot);
    if(!detail::Finite(hold.pivot)||!detail::Finite(hold.foregrip)||fc2vr::tracking::DotProduct(d,d)>4) return false;
    output=hold;return true;
}
// Reconstruct only the controller frame that drove the captured native solve.
// The rifle uses its existing shortest-arc span rotation about the right palm;
// a pistol keeps left wrist orientation while its support point rides the gun.
inline bool SupportedControllers(const Matrix& right,const Matrix& left,const SupportHold& hold,Matrix& outRight,Matrix& outLeft) {
    using namespace fc2vr::tracking;
    if(!detail::Rigid(right)||!detail::Rigid(left)||!detail::Finite(hold.pivot)||!detail::Finite(hold.foregrip)) return false;
    auto r=right,l=left;
    if(hold.mode==SupportMode::Pistol || hold.mode==SupportMode::PistolGrip) {
        if(hold.mode==SupportMode::PistolGrip) l=right;
        const auto point=PosePoint(right,hold.foregrip);
        l[12]=point.x;l[13]=point.y;l[14]=point.z;
    } else if(hold.mode==SupportMode::Rifle) {
        const auto pivot=PosePoint(right,hold.pivot);
        auto from=Subtract(PosePoint(right,hold.foregrip),pivot);
        auto to=Subtract({left[12],left[13],left[14]},pivot);
        const float a=DotProduct(from,from),b=DotProduct(to,to);
        if(!std::isfinite(a)||!std::isfinite(b)||a<.0004f||b<.0004f||a>4||b>4) return false;
        from=Scale(from,1/std::sqrt(a));to=Scale(to,1/std::sqrt(b));
        const auto cross=[](Vec3 x,Vec3 y) { return Vec3{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x}; };
        auto axis=cross(from,to);const float sine=std::sqrt(DotProduct(axis,axis));
        const float cosine=DotProduct(from,to);
        if(cosine<-.995f) return false; // an opposite/near-zero span has no stable short arc
        if(sine>1e-5f) {
            axis=Scale(axis,1/sine);
            for(unsigned row=0;row<3;++row) {
                const Vec3 v{right[row*4],right[row*4+1],right[row*4+2]};
                const auto rotated=Add(Add(Scale(v,cosine),Scale(cross(axis,v),sine)),Scale(axis,DotProduct(axis,v)*(1-cosine)));
                r[row*4]=rotated.x;r[row*4+1]=rotated.y;r[row*4+2]=rotated.z;
            }
        }
        r[12]=pivot.x;r[13]=pivot.y;r[14]=pivot.z;l=r;
    }
    if(!detail::Rigid(r)||!detail::Rigid(l)) return false;
    outRight=r;outLeft=l;return true;
}
} // namespace fc2vr::presentation
