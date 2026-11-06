#pragma once
#include "pose_authority.h"
namespace fc2vr::melee_motion {
// Never interpret changing physical hands, weapons or tracking leases as a
// fast blade movement. The first sample in the new context establishes a rest.
inline bool Continuous(std::uintptr_t previousWeapon,std::uintptr_t weapon,
    std::uint64_t previousLayout,std::uint64_t layout,const authority::Stamp& previous,
    const authority::Stamp& current,int hand,double seconds){
    return weapon && previousWeapon==weapon && previousLayout==layout
        && std::isfinite(seconds) && seconds>0 && seconds<=.25
        && authority::SameContext(previous,current,hand);
}
}
