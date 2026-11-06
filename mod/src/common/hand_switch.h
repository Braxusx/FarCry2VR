#pragma once
namespace fc2vr::hands {
// One transition per press. A second press cancels a queued busy transition.
// Observe key-up even when unfocused; returning to the game with F1 held must
// not manufacture a press. Losing focus cancels any pending request.
struct ToggleKey {
    bool held=false,pending=false;
    void Poll(bool down,bool allowed){
        const bool edge=down && !held;held=down;
        if(!allowed){pending=false;return;}
        if(edge)pending=!pending;
    }
    void Complete(){pending=false;}
};
}
