#pragma once

// Physical XR indices remain 0=left, 1=right. Existing arm/interaction frames
// use role indices 0=support, 1=weapon. Never remap the raw XR authority stamp.
namespace fc2vr::hands {
inline bool leftHanded = false; // configured once, before input hooks start
inline void Configure(bool left) { leftHanded = left; }
inline int Physical(int role) { return role < 0 ? role : role ^ int(leftHanded); }
inline int Weapon() { return Physical(1); }
inline int Support() { return Physical(0); }

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
