#pragma once
#include "support_pose.h"

namespace fc2vr::presentation {
// The captured bilateral rig has matching finger lengths and knuckle positions,
// reflected across wrist-local Z. Conjugation preserves positive handedness;
// reflecting just the translation would turn the skin basis inside out.
inline Matrix MirrorFinger(const Matrix& rightLocal) {
    auto out=rightLocal;
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
        if((r==2)!=(c==2)) out[r*4+c]=-out[r*4+c];
    return out;
}
inline Vec3 PistolGripContact(const Matrix& mainHand,Vec3 palm) {
    // Keep the accepted opposed grip pose. 55 mm between palm centres adds
    // 15 mm of clearance to candidate20's visibly interpenetrating hands.
    palm.y+=.055f;return PosePoint(mainHand,palm);
}
inline bool PistolGripHand(const Matrix& mainHand,Vec3 supportPalm,Vec3 contact,Matrix& out) {
    // Same finger direction, opposite palm normal, thumbs on the same side.
    auto oppose=Identity();oppose[5]=oppose[10]=-1;
    return PlaceSupportPalm(Multiply(oppose,mainHand),supportPalm,contact,out);
}
} // namespace fc2vr::presentation
