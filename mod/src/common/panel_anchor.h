#pragma once
#include "pose_frame.h"
#include <cstdint>
namespace fc2vr::tracking {
// A widescreen UI stretched into an eye-sized render target still has a
// widescreen logical layout. Texture dimensions are only the legacy fallback.
inline float PanelHeight(float width,float contentAspect,float textureAspect) {
    const float aspect=std::isfinite(contentAspect) && contentAspect>0 ? contentAspect : textureAspect;
    return width/(std::isfinite(aspect) && aspect>0 ? aspect : 16.0f/9.0f);
}
// OpenXR LOCAL coordinates. A new panel mode/recenter chooses an anchor once;
// subsequent head motion never drags the screen around the room.
struct PanelAnchor {
    Vec3 position{};
    float yaw=0;
    bool valid=false;
    int mode=-1;
    std::uint64_t generation=0;
    void Reset() { valid=false; }
    void Update(Vec3 head,Vec3 forward,std::uint64_t space,int kind,float distance,float vertical=0) {
        if (valid && space==generation && kind==mode) return;
        if (!std::isfinite(head.x) || !std::isfinite(head.y) || !std::isfinite(head.z)
            || !std::isfinite(forward.x) || !std::isfinite(forward.z)
            || !std::isfinite(distance) || distance<=0 || !std::isfinite(vertical)) return;
        const float n=std::sqrt(forward.x*forward.x+forward.z*forward.z);
        if (n<.2f) forward={0,0,-1}; else forward={forward.x/n,0,forward.z/n};
        position=Add(head,Scale(forward,distance)); position.y+=vertical;
        yaw=std::atan2(-forward.x,-forward.z);
        mode=kind; generation=space; valid=true;
    }
};
} // namespace fc2vr::tracking
