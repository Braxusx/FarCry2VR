// afw_fc2.h - Alternate Frame Warping (experimental), after the Dishonored VR mod's AFW
// (F:\Dishonored\AFW_RENDERING_GUIDE.md, by the Dishonored VR author; the design, not its code).
//
// The game renders ONE eye per frame, alternating left and right. The other (held) eye is rebuilt for
// this frame's head pose from two sources, on the game's own D3D10 device:
//   - the held eye's own previous image and linear depth: the world, carried to its new view with the
//     game's own view-projection matrices (exact in Far Cry 2: the mod builds every eye camera itself),
//     so walking, turning and head motion all come out right;
//   - the fresh eye's image and depth: the hands and weapon (they move by themselves), and any world the
//     held image could not see.
// Each source is drawn into the held view as a depth-tested grid mesh displaced by its depth (one vertex
// every AfwGridStep texels). Stretched triangles across depth breaks are dropped and filled from the
// other source; what neither source shows is filled from the farthest covered neighbour along the row.
// FC2 specifics: the linear depth is in metres (the engine's "Linear depth" target); the hands and
// weapon are told apart by depth (nearer than AfwHandDepth).
#pragma once
#include <stdint.h>
struct ID3D10Device;
struct ID3D10Texture2D;

namespace afw {
struct Config {
    int enable = 0;            // Afw
    int gridStep = 2;          // AfwGridStep: source texels per mesh vertex
    float handDepth = 0.3f;    // AfwHandDepth: nearer than this (stored depth) counts as the hands / weapon (FC2 stores them at 0.1-0.25)
    float handMin = 0.30f;     // AfwHandMinDepth: depth used for hand texels nearer than this (m)
    int handMotion = 2;        // AfwHandMotion: the held eye's hands follow the controllers on screen: 0 off, 1 move, 2 move + turn
    int hands = 0;             // AfwHands: 0 = the held eye's own hands, fixed to the view; 1 = from the fresh eye by depth
    int debug = 0;             // AfwDebug: 1 = tint the rebuilt eye by source (green fresh, none held, magenta fill)
};
void Configure(const Config& c);
bool Wanted();                 // enabled and not failed
void Toggle();
void CycleDebug();
// After eye e was rendered: keep its image, linear depth and unjittered view-projection (row vectors) as
// the source for rebuilding it later. frame = the mod's frame counter.
void Store(ID3D10Device* dev, int e, ID3D10Texture2D* color, ID3D10Texture2D* linDepth, const double vp[16],
           float camNear, float camFar, uint32_t frame, const float grips[2][6] = nullptr);   // grips: game space, [0] left [1] right
// Is there a recent enough image of eye e to rebuild it this frame?
bool HasSource(int e, uint32_t frame);
// Rebuild eye `held` for the view-projection vpTarget (this frame), from its own stored image and the
// fresh eye's. Returns the rebuilt image (eye size, same format as the eye image) or null.
ID3D10Texture2D* Rebuild(ID3D10Device* dev, int held, int fresh, const double vpTarget[16], ID3D10Texture2D* heldColor = nullptr,
                         const float gripsNow[2][6] = nullptr);
void Invalidate();             // drop both sources (a cut, a load, a mode change)
}
