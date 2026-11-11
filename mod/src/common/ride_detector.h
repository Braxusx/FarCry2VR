#pragma once
#include "tracking_frame.h"
#include <cmath>
#include <cstdint>
namespace fc2vr::tracking {
// Passive rides use the default action map in the intro. Detect small coherent
// entity motion without stick input, then retain the seat through traffic stops.
class RideDetector {
    bool have=false, active=false;
    std::uintptr_t skeleton=0,graphics=0;
    Vec3 previous{},start{};
    double last=0,window=0,blockedUntil=0;
    float seatedHeight=0;
public:
    bool Update(double now,Vec3 pos,std::uintptr_t skel,std::uintptr_t gfx,bool valid,
                bool stick,bool explicitVehicle,bool blocked,float height) {
        if (!valid || blocked || !skel || !gfx || !std::isfinite(now) || !std::isfinite(height)
            || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) { *this={}; return false; }
        if (!have || skel!=skeleton || gfx!=graphics) {
            *this={}; have=true; skeleton=skel; graphics=gfx; previous=start=pos; last=window=now;
            active=explicitVehicle; seatedHeight=height; return active;
        }
        const double dt=(now-last)*.001; last=now;
        const float dx=pos.x-previous.x,dy=pos.y-previous.y; previous=pos;
        if (explicitVehicle) { active=true; seatedHeight=height; start=pos; window=now; return true; }
        if (stick) { active=false; start=pos; window=now; blockedUntil=now+300; return false; }
        if (active && height>seatedHeight+.25f) { // standing up / leaving this seat
            active=false; start=pos; window=now; blockedUntil=now+300; return false;
        }
        if (dt<=0 || dt>.25) { start=pos; window=now; return active; }
        if (dx*dx+dy*dy>static_cast<float>(3600*dt*dt)) {
            active=false; start=pos; window=now; blockedUntil=now+150; return false;
        }
        if (active) return true; // zero speed is still the same ride
        if (now<blockedUntil || now-window>150) { start=pos; window=now; return false; }
        const float x=pos.x-start.x,y=pos.y-start.y;
        if (now-window>=10 && x*x+y*y>=.004f*.004f) { active=true; seatedHeight=height; }
        return active;
    }
};
} // namespace fc2vr::tracking
