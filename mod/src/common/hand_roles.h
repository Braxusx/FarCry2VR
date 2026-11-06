#pragma once
#include <utility>

// Physical XR indices remain 0=left, 1=right. Existing arm/interaction frames
// use role indices 0=support, 1=weapon. Never remap the raw XR authority stamp.
namespace fc2vr::hands {
inline bool leftHanded = false; // configured once, before input hooks start
inline void Configure(bool left) { leftHanded = left; }
inline int Physical(int role) { return role < 0 ? role : role ^ int(leftHanded); }
inline int Weapon() { return Physical(1); }
inline int Support() { return Physical(0); }

inline bool WeaponRig(bool weapon, bool map, bool normalControls, bool nativeLeft) {
    return leftHanded && weapon && !map && normalControls && nativeLeft;
}

// Item animations retain their native hand attachments (map, radio, phone,
// monocular, empty/scripted hands). Convert only the arm solve's local copy;
// action routing and physical XR source stamps remain unchanged.
template<class Frame> void AnatomicalItems(Frame& f) {
    std::swap(f.handValid,f.leftValid);
    std::swap(f.handAuthority,f.leftAuthority);
    std::swap(f.rSqueeze,f.lSqueeze);
    for(int i=0;i<3;++i) {
        std::swap(f.aimO[i],f.laimO[i]);
        std::swap(f.aimR[i],f.laimR[i]);
        std::swap(f.aimU[i],f.laimU[i]);
        std::swap(f.aimB[i],f.laimB[i]);
        std::swap(f.gripO[i],f.lgripO[i]);
    }
    f.twoHand=false;
}

template<class Hand> void Controls(const Hand* physical, Hand* roles) {
    roles[0] = physical[Support()];
    roles[1] = physical[Weapon()];
    // Locomotion, face buttons and pause retain their physical locations.
    // Only poses, triggers and grips travel with their weapon/support role.
    for (int i=0;i<2;++i) {
        roles[i].stick = physical[i].stick;
        roles[i].stickClick = physical[i].stickClick;
        roles[i].menu = physical[i].menu;
        roles[i].btn1 = physical[i].btn1;
        roles[i].btn2 = physical[i].btn2;
    }
}
} // namespace fc2vr::hands
