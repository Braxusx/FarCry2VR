#pragma once

// XR LOCAL -> recentered tracking coordinates (x right, y up, -z forward).
// Translation belongs to positions, never camera axes or other directions.
// Caller supplies DoRecenter's horizontal, unit right/forward basis (yaw only).
namespace fc2vr::tracking {
struct Vec3 { float x, y, z; };

struct ReferenceFrame {
    Vec3 origin{ 0, 0, 0 };
    Vec3 right{ 1, 0, 0 };
    Vec3 forward{ 0, 0, -1 };
    float heightOffset = 0;

    Vec3 Direction(Vec3 v) const {
        return { v.x * right.x + v.y * right.y + v.z * right.z,
                 v.y,
                 -(v.x * forward.x + v.y * forward.y + v.z * forward.z) };
    }

    Vec3 Position(Vec3 p) const {
        Vec3 v = Direction({ p.x - origin.x, p.y - origin.y, p.z - origin.z });
        v.y += heightOffset;
        return v;
    }

    Vec3 LocalDirection(Vec3 r) const {
        return {right.x*r.x-forward.x*r.z,
                r.y,
                right.z*r.x-forward.z*r.z};
    }

    Vec3 LocalPosition(Vec3 p) const {
        Vec3 v = LocalDirection({p.x,p.y-heightOffset,p.z});
        return {v.x+origin.x,v.y+origin.y,v.z+origin.z};
    }
};
} // namespace fc2vr::tracking
