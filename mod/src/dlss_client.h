// dlss_client.h - FC2VR: taken from the Dishonored VR mod (core/gfx/dlss_client.h, used with permission);
// changes: namespace, the depth convention is a parameter of build(). Original text follows.
//
// core/gfx/dlss_client.h - the 32-bit side of the DLSS transport: starts the x64 NGX helper
// (src/tools/dlss_host), shares one set of textures and a fence pair per eye with it, and
// runs one eye image through its DLSS feature. No mod dependencies (logging goes through a
// callback), so tools/dlss-host-test.cpp drives the real 32-to-64 path with synthetic
// images. The mod side - settings, guides, when to reset, where the output goes - is
// core/gfx/dlss.cpp. The wire contract and the design credit are in dlss_ipc.h.
//
// Threads: start() and build() block (process launch, NGX init, feature creation take
// hundreds of milliseconds) and use only the device, never a context, so the mod runs them
// on a worker. evaluate() uses the immediate context and belongs to the present thread.
// The mod's state machine never lets the two overlap.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <dxgiformat.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace fc2vr::dlss {

// 0 info, 1 warn, 2 error
using LogFn = void (*)(int level, const char* line);

struct StartParams {
    const wchar_t* hostExe = nullptr;   // dvr_dlss_host64.exe; nvngx_dlss.dll beside it
    const wchar_t* dataDir = nullptr;   // the helper's log and NGX's cache/logs
    LogFn log = nullptr;
    uint32_t timeoutMs = 20000;         // launch + NGX init
    uint32_t backend = 0;               // dlss_ipc::Backend: 0 DLSS (NGX), 1 FSR (FidelityFX API)
    uint32_t fsrVersion = 0;            // FSR: 0 = the runtime's default, else 1-based in its list
};

struct EyeInputs {
    ID3D11Texture2D* color = nullptr;   // render size, B8G8R8A8/R8G8B8A8 family (gamma LDR)
    ID3D11Texture2D* depth = nullptr;   // render size, R32_FLOAT, reversed (1 near, 0 far/sky)
    ID3D11Texture2D* motion = nullptr;  // render size, R16G16_FLOAT, previous UV minus current UV
    ID3D11Texture2D* bias = nullptr;    // optional, render size, R8_UNORM: 1 = take the current colour
    bool reset = false;
    float jitterX = 0, jitterY = 0;     // render pixels, already in the backend's sign (dlss_jitter.h)
    float mvSign = 1;                   // the motion texture's sign as the backend reads it
    float sharpness = 0;
    // For FSR's depth reconstruction (DLSS ignores them): see dlss_ipc.h Frame.
    float frameTimeMs = 0, fovY = 0, metersPerUnit = 0;
    // Called on the context right after the inputs are copied, before the DLSS wait is queued:
    // the capture and depth slots can be released there, so the game's next frame never waits
    // for this image's DLSS (the overlap).
    void (*afterCopy)(ID3D11DeviceContext* ctx) = nullptr;
};

struct Stats {
    uint64_t frames[2] = {}, refused[2] = {};
    double cpuMsSum[2] = {};            // submit to output-ready, present thread
    double gpuMsSum[2] = {}; uint64_t gpuN[2] = {};   // the helper's evaluate timestamps
    double cpuMsMax[2] = {};
};

class Client {
public:
    ~Client() { stop(); }
    // Launches the helper on this device's adapter and completes the handshake. False with
    // a reason; the helper is gone again.
    bool start(ID3D11Device* dev, const StartParams& sp, char* why, size_t cap);
    // (Re)creates one eye's shared set at w x h -> ow x oh (equal = DLAA) and its DLSS
    // feature. colorFormat is the source image's format.
    bool build(int eye, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, DXGI_FORMAT colorFormat,
               int preset, char* why, size_t cap, bool depthInverted = true, bool autoExposure = true);
    // One eye image. On success output(eye) holds the result, ordered on the context after
    // the helper's work. False with a reason: nothing usable was produced (the caller takes
    // its normal path); a transport failure also stops the client (running() goes false).
    bool evaluate(ID3D11DeviceContext* ctx, int eye, const EyeInputs& in, char* why, size_t cap);
    ID3D11ShaderResourceView* output(int eye) const;
    ID3D11Texture2D* output_texture(int eye) const;
    bool built(int eye, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, DXGI_FORMAT colorFormat) const;
    bool running() const;
    void stop();
    const char* adapter() const { return adapter_; }
    const char* runtime() const { return runtime_; }    // FSR: the provider in use ("FSR 3.1.3")
    const char* offered() const { return offered_; }    // FSR: every version the runtime offers
    uint32_t backend() const { return backend_; }
    uint64_t bytes() const { return bytes_; }     // shared texture memory held (both eyes)
    Stats stats;

private:
    struct Eye;
    bool send(const void* p, uint32_t n, uint32_t ms);
    bool recv(void* p, uint32_t n, uint32_t ms);
    void release_eye(int eye);
    void say(int level, const char* fmt, ...);
    void fail(const char* fmt, ...);

    ID3D11Device* dev_ = nullptr;
    void* dev5_ = nullptr;     // ID3D11Device5
    void* process_ = nullptr;  // HANDLE
    void* job_ = nullptr;
    void* pipe_ = nullptr;
    void* event_ = nullptr;
    Eye* eyes_[2] = {};
    LogFn log_ = nullptr;
    char adapter_[128] = "";
    char runtime_[64] = "";
    char offered_[128] = "";
    uint32_t backend_ = 0;
    uint64_t bytes_ = 0;
    bool running_ = false;
};

} // namespace fc2vr::dlss
