#pragma once
namespace fc2vr::input {
struct DrivingButtons { bool handbrake=false,changeSeat=false,allowRecenter=false; };
// The original grip/A layout remains available with both options off. Motion
// steering reserves grips; the A-handbrake layout is an independent preference.
inline DrivingButtons DrivingLayout(bool motion,bool handbrakeA,bool leftClick,bool a,float leftGrip,float rightGrip) {
    DrivingButtons out;
    out.handbrake=(!motion && leftGrip>.5f) || (handbrakeA ? a : motion && leftClick);
    out.changeSeat=(!motion && rightGrip>.6f) || (handbrakeA ? leftClick : a);
    out.allowRecenter=!motion && !handbrakeA;
    return out;
}
}
