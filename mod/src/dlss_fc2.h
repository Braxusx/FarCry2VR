// dlss_fc2.h - NVIDIA DLSS / DLAA for the Far Cry 2 VR eye images.
//
// NGX is 64-bit only and Far Cry 2 is 32-bit, so DLSS runs in a 64-bit helper process (dlss_host/,
// taken from the Dishonored VR mod) and the eye images are shared with it (dlss_client.h).
//
// Per eye, right after the eye is rendered (game D3D10 device):
//   PrepareEye10: the engine's depth buffer is copied, and one full-screen pass writes the DLSS
//   guides into textures shared with the mod's D3D11 device: depth (R32F, standard 0 near / 1 far),
//   camera motion vectors (R16G16F, previous UV minus current UV, from depth and the previous frame's
//   view-projection of this eye) and the bias mask (R8, 1 = the first-person arms/weapon near pass,
//   whose motion the camera vectors don't describe).
// At the OpenXR submit (D3D11), Evaluate11 runs the eye's DLSS feature and returns the result, which
// is copied into the swapchain instead of the plain eye image.
//
// Jitter: the eye's projection is shifted by a Halton (2,3) sub-pixel offset per frame (main.cpp, through
// the eye tangents); the guides use the unjittered matrices, and the offset goes to DLSS.
#pragma once
#include <stdint.h>
struct ID3D10Device;
struct ID3D10Texture2D;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace dlssfc2 {
struct Config {
    int enable = 0;          // Dlss
    int preset = 11;         // DlssPreset: NVSDK_NGX_DLSS_Hint_Render_Preset (10 J, 11 K, 12 L, 13 M; 16 = NVIDIA's pick per mode)
    int quality = 0;         // DlssQuality: 0 DLAA (native), 1 Ultra Quality, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance
    int jitter = 1;          // DlssJitter
    int phases = 16;         // DlssJitterPhases
    int mask = 1;            // DlssMask: the near pass (arms, weapon) takes the current colour
    float sharpness = 0.0f;  // DlssSharpness (deprecated in DLSS 3+, usually ignored)
    int autoExposure = 0;    // DlssAutoExposure: 1 = NGX computes an exposure (for HDR input; the eye image is LDR)
    int debug = 0;
};
void Configure(const Config& c, const char* binDir);
bool Wanted();               // enabled and not failed (main jitters the projection only then)
bool Active();               // the helper is up and both eyes are built
void Toggle();               // hotkey: on/off live
void CycleDebug();           // hotkey: jitter sign / no jitter / zero vectors (diagnostics)
void SetNotify(void (*fn)(const char* text));   // on-screen notes (mode changes)
void CyclePreset();          // hotkey: K -> J -> M -> L -> NVIDIA's pick -> K
// Upscaling: the eye render size relative to the shown (output) size for DlssQuality (1 = DLAA or DLSS off).
float RenderRatio();
// The shown eye size when the eyes render smaller (upscaling), 0 = DLAA (same size).
void SetOutputSize(uint32_t w, uint32_t h);
const char* QualityName(int q);
void ArmCapture();           // with the F7 capture: the next eye 0 DLSS inputs and output to bmp files
// Main thread, start of a stereo frame: picks this frame's jitter offset (render pixels, +x right,
// +y down: the pixel shows the scene at (x + 0.5 + sx, y + 0.5 + sy)).
void BeginFrame(float* sx, float* sy);
// After eye e is rendered. vpCur/vpPrev: this eye's unjittered view-projection (row vectors, row major)
// of this frame and the previous one (vpPrev null = no history: reset). nearMaxDepth: the largest depth
// the near pass writes.
void PrepareEye10(ID3D10Device* dev, int e, ID3D10Texture2D* depthTex, const double vpCur[16], const double* vpPrev,
                  float nearMaxDepth, float jitterX, float jitterY, float camNear, float camFar, const float tan4[4]);
// The engine's linear depth colour target (R32F, found by main's census); used for the depth when set.
void SetLinearDepth(ID3D10Texture2D* t);
// Called by xr::EndFrame (D3D11) per eye after the D3D10 work is visible: the DLSS result, or null.
ID3D11Texture2D* Evaluate11(ID3D11Device* dev, ID3D11DeviceContext* ctx, int e, ID3D11Texture2D* color);
void Shutdown();
}
