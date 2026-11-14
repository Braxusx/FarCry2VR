#pragma once
#include "pose_frame.h"

namespace fc2vr::tracking {
// Keep the usual leaning allowance in front/to the side, but do not leave the
// eyes ten centimetres behind the torso when physically backing up. The caller
// still limits speed and collision-tests every requested body move. Inputs are
// world directions/offsets (Dunia Z up), not camera positions or stick intent.
inline float RoomScaleLeash(Vec3 offset, Vec3 headForward, float leash, float backLeash=.02f) {
    // Invalid configuration/direction keeps the ordinary leash; do not infer a
    // rear direction while looking almost vertically or from a stale pose.
    if (!std::isfinite(leash) || leash<0 || !std::isfinite(backLeash) || backLeash<0) return leash;
    const float distance2=offset.x*offset.x+offset.y*offset.y;
    const float forward2=headForward.x*headForward.x+headForward.y*headForward.y;
    if (!std::isfinite(distance2) || !std::isfinite(forward2) || distance2<1e-12f || forward2<.04f) return leash;
    const float rear=-(offset.x*headForward.x+offset.y*headForward.y)/std::sqrt(forward2);
    const float limit=backLeash<leash?backLeash:leash;
    if (!std::isfinite(rear) || rear<=limit) return leash;
    // Intersect the radial lean circle with a plane behind the current head yaw.
    // The residual backward projection is bounded without pulling sideways or
    // forward leans into the smaller radius. Both boundaries meet continuously.
    const float directional=limit*std::sqrt(distance2)/rear;
    return directional<leash?directional:leash;
}

// Caller serializes this ledger. Totals are recentered tracking coordinates in
// game units, not world axes: a delayed acknowledgement must survive stick yaw.
class RoomScaleLedger {
    Vec3 total{},taken{},offset{},acknowledged{};
    GameBasis basis{};
    std::uint64_t epoch=1;
    bool valid=false;
public:
    std::uint64_t Epoch() const { return epoch; }
    Vec3 Total() const { return total; }
    void Reset() { total=taken=offset=acknowledged={};valid=false;++epoch; }
    void Request(Vec3 worldOffset, const GameBasis& game, Vec3 applied, std::uint64_t generation, bool enabled) {
        valid=enabled && generation==epoch;offset=worldOffset;basis=game;acknowledged=applied;
    }
    Vec3 Remaining() const {
        return valid ? Subtract(offset,basis.Direction(Subtract(total,acknowledged))) : Vec3{};
    }
    // Use the basis of the request which produced the collision-tested move.
    // A newer render request may have arrived while the native move ran.
    GameBasis RequestBasis() const { return basis; }
    void Commit(Vec3 worldMove, const GameBasis& request, std::uint64_t generation) {
        if(generation==epoch) total=Add(total,request.ReferenceDirection(worldMove));
    }
    Vec3 Take() { const Vec3 delta=Subtract(total,taken);taken=total;return delta; }
};
}
