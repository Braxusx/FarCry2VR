// xr.h - OpenXR runtime + D3D11 interop for the FC2 VR mod (game renders with D3D10.1).
#pragma once
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "common/pose_authority.h"

namespace xr {

struct FrameViews {
    bool    valid = false;        // views located with valid orientation
    bool    positionValid = false; // finite valid eye positions; required before recenter/height/6DoF use
    bool    shouldRender = false;
    XrTime  displayTime = 0;
    uint64_t spaceGeneration = 0;
    XrView  views[2]{};           // LOCAL space, per eye
};

// Two-phase init on the render thread.
// 1) instance/system/session; returns the runtime-recommended per-eye size.
bool InitInstance(ID3D10Device* dev10, uint32_t& recW, uint32_t& recH);
// 2) swapchains + shared textures for the eye render targets and the backbuffer (screen quad).
// DLSS upscaling: the eye swapchains are w x h (the output) while the eyes render smaller; call before InitSwapchains
void SetEyeOutputSize(UINT w, UINT h);
bool InitSwapchains(const D3D10_TEXTURE2D_DESC& eyeDesc, const D3D10_TEXTURE2D_DESC& bbDesc);
bool Ready();             // instance + session + swapchains created
bool Fatal();             // init failed in a way retrying will not fix
bool SessionRunning();    // session in a state where frames should be submitted
void PollEvents();

// Stereo frame: Begin -> CopyEye(0), CopyEye(1) -> End
bool BeginFrame(FrameViews& out);       // xrWaitFrame + xrBeginFrame + xrLocateViews
void CopyEye(int eye, ID3D10Texture2D* src);
void EndFrame(const FrameViews& fv, bool submittedEyes);
// Part of eye e's swapchain image to show this frame (pixels); reset by EndFrame.
void SetEyeRect(int e, int x, int y, int w, int h);
// Called at the submit for each eye image (D3D11, after the game's D3D10 work is visible): return a texture to
// show instead of it (same size and format family), or null for the eye image itself.
typedef ID3D11Texture2D* (*EyeFilter)(ID3D11Device* dev, ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* eyeImage);
void SetEyeFilter(EyeFilter f);
// AFW with DLSS: copy each filtered (DLSS) result back into the eye's shared texture, so the game device can rebuild
// that eye from what it showed. EyeTexture10: that texture on the game's device; WaitEyeCopies before reading it.
void SetCopyBack(bool on);
ID3D10Texture2D* EyeTexture10(int e);
void WaitEyeCopies();

// Flat "virtual screen" frame for menus/loading (whole frame as a quad layer).
// pose: quad centre/orientation in LOCAL space (null = 2 m ahead of the LOCAL origin); width in metres.
void SubmitScreen(ID3D10Texture2D* src, const XrPosef* pose = nullptr, float width = 2.4f);
// End the current (BeginFrame) frame showing only src as a flat quad at pose (LOCAL), width metres.
void EndFrameScreen(const FrameViews& fv, ID3D10Texture2D* src, const XrPosef& pose, float width);
// Scope: square centre crop of src in a round window with a crosshair; hudRect (x,y,w,h px of the HUD texture) shown at hudPose.
void EndFrameScope(const FrameViews& fv, ID3D10Texture2D* src, const XrPosef& pose, float width, const float reticleRect[4], const float ammoRect[4], const XrPosef& ammoPose, float ammoWidth, bool srcResolved = false, bool ownAmmo = false, bool plain = false);

// HUD layer: an eye-sized transparent texture on the game device that the game's UI is redirected into.
ID3D10Texture2D* HudTexture();
// Show the HUD as a quad this frame (call between BeginFrame and EndFrame). pose in LOCAL space, width in metres.
void SetHudLayer(const XrPosef& pose, float width, bool viewSpace = false);   // viewSpace: pose relative to the head

// Reticle layer: eye-sized texture; the central `frac` of it is shown as a quad (LOCAL space), `width` metres wide.
ID3D10Texture2D* ReticleTexture();
void SetReticleLayer(const XrPosef& pose, float width, float frac);
// Aiming dot (a small round quad), LOCAL space, size in metres; call each frame it should show.
void SetDotLayer(const XrPosef& pose, float size);
// Throwing arc: points in LOCAL space (metres), drawn into both eye images this frame (no depth test). land = draw a
// landing ring at the last point (normal = its surface normal, LOCAL); ok = the throw would work (white) or not (red).
void SetArc(const XrVector3f* pts, int n, bool land, const XrVector3f& normal, bool ok);
constexpr int MenuW = 512, MenuH = 320;
constexpr int AmmoW = 384, AmmoH = 96;
void SetAmmoImage(const uint32_t* px);   // the mod's own ammo counter (RGBA premultiplied, AmmoW x AmmoH)
void SetMenu(const uint32_t* px, const XrPosef& pose, float width);   // this frame: show the mod menu (px: new RGBA premultiplied pixels, or null to keep)
void SetScopeCrosshair(int style);
void SetMonoTarget(bool on);
void SetHudTopBand(float band, float drop);
void SetHudTopBandOn(bool on);   // v228: only in gameplay (not menus / loading screens)   // v227: HUD top strip (part of the height) shown lowered by drop (part of the height)
void SetMonoRingSize(float k);   // v218: the ring's radius as a part of the window's (call before Init)   // v217: the monocular's ring green (something taggable in it) or red
void SetMonoViewScale(float k);   // v216: the monocular view is cropped this much tighter (lens fills the window)   // 0 none, 1 duplex, 2 duplex with mil dots (call before Init)

// Motion controllers (synced in BeginFrame; poses in LOCAL space at the predicted display time). side 0 = left, 1 = right.
struct HandState {
    // gripValid/aimValid describe the visual pose (including HoldLostTracking).
    // Only poseAuthority plus raw tracked evidence can authorize physical use.
    bool active = false, gripValid = false, aimValid = false;
    fc2vr::authority::PoseEvidence gripEvidence{}, aimEvidence{};
    bool gripHeld = false, aimHeld = false, poseAuthority = false;
    XrPosef grip{ { 0, 0, 0, 1 }, { 0, 0, 0 } }, aim{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
    float trigger = 0, squeeze = 0;
    XrVector2f stick{ 0, 0 };
    bool stickClick = false, btn1 = false /* A or X */, btn2 = false /* B or Y */, menu = false /* left only */;
};
const HandState& Hand(int side);   // render thread only; visual fallback, never gameplay authority
void Haptic(int side, float amplitude, float seconds, float frequency = 0);   // amplitude 0 stops
// Thread-safe physical snapshot: checks focus/sync/receipt age, masks pose flags
// to tracked/rearmed authority, and clears failed head validity. Buttons are independent.
using InputStamp = fc2vr::authority::Stamp;
bool InputSnapshot(HandState out[2], XrPosef& head, bool& headValid, InputStamp* stamp = nullptr);
InputStamp CurrentInputStamp();
bool InputCurrent(const InputStamp& source, int hand = -1);
bool InputCurrent();
bool HeadCurrent(const InputStamp& source);
bool AcceptedInputCurrent(const InputStamp& source, int hand);   // accepted snapshot, not permission to refresh a raw pose
void InvalidateForRecenter();   // render thread; invalidates leases and requires neutral controls again
void SetRumble(float left, float right);
void SetHoldLostPose(bool on);   // HoldLostTracking: keep the last pose of a controller that loses tracking   // 0..1 per hand, applied every frame

void Shutdown();
const char* LastError();

} // namespace xr
