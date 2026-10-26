#pragma once

namespace fc2vr::startup {
// XR tracking is not evidence of a native gameplay camera. In particular, the
// front end renders a viewport before there is a pawn. Keep that viewport native
// and single-pass; Present already submits the completed backbuffer as a screen.
// No latch: returning to menus/loading revokes world-camera ownership immediately.
inline bool WorldCamera(bool playerObserved, bool loading, bool menu) {
    return playerObserved && !loading && !menu;
}
}
