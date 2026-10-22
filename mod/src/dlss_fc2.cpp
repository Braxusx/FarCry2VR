// dlss_fc2.cpp - see dlss_fc2.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dlss_fc2.h"
#include "dlss_client.h"
#include "log.h"

namespace dlssfc2 {
namespace {
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
Config g_c;
wchar_t g_dir[MAX_PATH] = L"";        // <bin>\fc2vr_dlss
bool g_failed = false, g_on = false;
uint32_t g_phase = 0;
fc2vr::dlss::Client g_client;
bool g_started = false;

// ---------------------------------------------------------------- D3D10: the guides pass
const char kHlsl[] = R"(
Texture2D<float> gDepth : register(t0);
Texture2D<float> gLin : register(t1);
cbuffer C : register(b0) { row_major float4x4 M; float2 size; float nearMax; float useMask; float2 jit; float hist; float pad;
                           float linMode; float linScale; float P10; float P14; float4 tanR; float radial; float camNear; float2 pad2; };
struct VSO { float4 pos : SV_Position; };
VSO vs(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSO o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
struct PSO { float d : SV_Target0; float2 mv : SV_Target1; float b : SV_Target2; };
PSO ps(VSO i) {
    int2 p = int2(i.pos.xy);
    float d = gDepth.Load(int3(p, 0));
    bool nearPass = false;
    if (linMode > 0.5) {   // the engine's linear depth target -> standard device depth of this projection
        float z = gLin.Load(int3(p, 0)) * linScale;
        float2 uvc = i.pos.xy / size;
        if (radial > 0.5) { float tx = lerp(tanR.x, tanR.y, uvc.x), ty = lerp(tanR.w, tanR.z, uvc.y); z /= sqrt(1 + tx * tx + ty * ty); }
        nearPass = z < camNear;   // the arms and weapon: drawn closer than the camera's near plane (their own projection)
        d = z > 0 ? saturate(-P10 + P14 / z) : 1;
    }
    // the unjittered position of this pixel's sample
    float2 uv = (i.pos.xy + jit) / size;
    float4 c = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 q = mul(c, M);
    float2 puv = uv;
    if (hist > 0.5 && abs(q.w) > 1e-12) { float2 n = q.xy / q.w; puv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5); }
    PSO o; o.d = d; o.mv = puv - uv; o.b = (useMask > 0.5 && (nearPass || d <= nearMax)) ? 1 : 0; return o;
}
)";
struct CB { float M[16]; float size[2]; float nearMax; float useMask; float jit[2]; float hist; float pad; float linMode, linScale, P10, P14; float tanR[4]; float radial, camNear, pad2[2]; };
ID3D10Texture2D* g_lin = nullptr; ID3D10ShaderResourceView* g_linSrv = nullptr; float g_linScale = -1;   // -1 = not decided

struct Eye10 {
    UINT w = 0, h = 0; DXGI_FORMAT depthFmt = DXGI_FORMAT_UNKNOWN;
    ID3D10Texture2D* depthCopy = nullptr; ID3D10ShaderResourceView* depthSrv = nullptr;
    ID3D10Texture2D* out[3] = {}; ID3D10RenderTargetView* rtv[3] = {}; HANDLE shared[3] = {};
    ID3D11Texture2D* out11[3] = {};   // opened on the D3D11 device
    bool fresh = false, reset = true; float jx = 0, jy = 0;
};
Eye10 g_e[2];
ID3D10VertexShader* g_vs = nullptr; ID3D10PixelShader* g_ps = nullptr; ID3D10Buffer* g_cb = nullptr;
ID3D10DepthStencilState* g_dss = nullptr; ID3D10RasterizerState* g_rs = nullptr; ID3D10BlendState* g_bs = nullptr;
bool g_init10 = false, g_bad10 = false;
typedef HRESULT (WINAPI* CreateSB_t)(ID3D10Device*, D3D10_STATE_BLOCK_MASK*, ID3D10StateBlock**);
typedef HRESULT (WINAPI* MaskAll_t)(D3D10_STATE_BLOCK_MASK*);
ID3D10StateBlock* g_sb = nullptr;

bool Init10(ID3D10Device* dev) {
    if (g_init10) return true;
    if (g_bad10) return false;
    g_bad10 = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!comp) { LOG("dlss: d3dcompiler_47.dll not available - DLSS off"); return false; }
    ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
    HRESULT hr = comp(kHlsl, sizeof(kHlsl) - 1, "fc2vr_dlss_guides", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bv, &err);
    if (FAILED(hr)) { LOG("dlss: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); Rel(err); return false; }
    hr = comp(kHlsl, sizeof(kHlsl) - 1, "fc2vr_dlss_guides", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bp, &err);
    if (FAILED(hr)) { LOG("dlss: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); Rel(err); Rel(bv); return false; }
    bool ok = SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), &g_ps));
    Rel(bv); Rel(bp);
    D3D10_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB); bd.Usage = D3D10_USAGE_DEFAULT; bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    D3D10_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_dss));
    D3D10_RASTERIZER_DESC rd = {}; rd.FillMode = D3D10_FILL_SOLID; rd.CullMode = D3D10_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    D3D10_BLEND_DESC bl = {}; for (int i = 0; i < 8; ++i) bl.RenderTargetWriteMask[i] = D3D10_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_bs));
    HMODULE d10 = GetModuleHandleW(L"d3d10.dll"); if (!d10) d10 = LoadLibraryW(L"d3d10.dll");
    CreateSB_t csb = d10 ? (CreateSB_t)GetProcAddress(d10, "D3D10CreateStateBlock") : nullptr;
    MaskAll_t mall = d10 ? (MaskAll_t)GetProcAddress(d10, "D3D10StateBlockMaskEnableAll") : nullptr;
    if (ok && csb && mall) { D3D10_STATE_BLOCK_MASK m; mall(&m); ok = SUCCEEDED(csb(dev, &m, &g_sb)); } else ok = false;
    if (!ok) { LOG("dlss: D3D10 guide pass setup failed - DLSS off"); return false; }
    g_bad10 = false; g_init10 = true;
    LOG("dlss: guide pass ready (depth, camera motion vectors, near-pass mask)");
    return true;
}
DXGI_FORMAT Typeless(DXGI_FORMAT f, DXGI_FORMAT* srv) {
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: *srv = DXGI_FORMAT_R32_FLOAT; return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: *srv = DXGI_FORMAT_R16_UNORM; return DXGI_FORMAT_R16_TYPELESS;
    default: *srv = DXGI_FORMAT_UNKNOWN; return DXGI_FORMAT_UNKNOWN;
    }
}
// reads the centre 64x64 of a depth-family texture through a staging copy; 24-bit (D24S8) or 32-bit float
bool DepthProbe(ID3D10Device* dev, ID3D10Texture2D* src, float* lo, float* hi, bool full = false) {
    D3D10_TEXTURE2D_DESC d; src->GetDesc(&d);
    D3D10_TEXTURE2D_DESC sd = d; sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D10_USAGE_STAGING; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    DXGI_FORMAT srv; DXGI_FORMAT tl = Typeless(d.Format, &srv); if (tl != DXGI_FORMAT_UNKNOWN) sd.Format = tl;
    ID3D10Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return false;
    dev->CopyResource(st, src);
    D3D10_MAPPED_TEXTURE2D m; bool ok = SUCCEEDED(st->Map(0, D3D10_MAP_READ, 0, &m));
    if (ok) {
        *lo = 1e30f; *hi = -1e30f;
        UINT y0 = full ? 0 : d.Height / 2 - 32, y1 = full ? d.Height : d.Height / 2 + 32, x0 = full ? 0 : d.Width / 2 - 32, x1 = full ? d.Width : d.Width / 2 + 32, st2 = full ? 4 : 1;
        for (UINT y = y0; y < y1; y += st2) for (UINT x = x0; x < x1; x += st2) {
            const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + x * 4; float v;
            if (tl == DXGI_FORMAT_R24G8_TYPELESS) v = (float)(*(const uint32_t*)p & 0xFFFFFF) / 16777215.0f; else memcpy(&v, p, 4);
            *lo = fminf(*lo, v); *hi = fmaxf(*hi, v);
        }
        st->Unmap(0);
    }
    st->Release(); return ok;
}
void Release10(Eye10& e) {
    Rel(e.depthSrv); Rel(e.depthCopy);
    for (int i = 0; i < 3; ++i) { Rel(e.rtv[i]); Rel(e.out[i]); Rel(e.out11[i]); e.shared[i] = nullptr; }
    e.w = e.h = 0;
}
bool Ensure10(ID3D10Device* dev, Eye10& e, ID3D10Texture2D* depth) {
    D3D10_TEXTURE2D_DESC dd; depth->GetDesc(&dd);
    if (e.w == dd.Width && e.h == dd.Height && e.depthFmt == dd.Format && e.depthCopy) return true;
    Release10(e);
    DXGI_FORMAT sf, tf = Typeless(dd.Format, &sf);
    LOG("dlss: eye depth %ux%u format %d samples %u bind 0x%x", dd.Width, dd.Height, (int)dd.Format, dd.SampleDesc.Count, dd.BindFlags);
    if (tf == DXGI_FORMAT_UNKNOWN || dd.SampleDesc.Count != 1) { LOG("dlss: unsupported depth buffer - DLSS off"); return false; }
    D3D10_TEXTURE2D_DESC c = dd; c.Format = tf; c.BindFlags = D3D10_BIND_SHADER_RESOURCE; c.MiscFlags = 0; c.CPUAccessFlags = 0; c.Usage = D3D10_USAGE_DEFAULT;
    if (FAILED(dev->CreateTexture2D(&c, nullptr, &e.depthCopy))) { LOG("dlss: depth copy texture failed"); return false; }
    D3D10_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = sf; sv.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(e.depthCopy, &sv, &e.depthSrv))) { LOG("dlss: depth SRV failed"); return false; }
    static const DXGI_FORMAT fm[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM };
    for (int i = 0; i < 3; ++i) {
        D3D10_TEXTURE2D_DESC o = {}; o.Width = dd.Width; o.Height = dd.Height; o.MipLevels = 1; o.ArraySize = 1; o.Format = fm[i];
        o.SampleDesc.Count = 1; o.Usage = D3D10_USAGE_DEFAULT; o.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE; o.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
        if (FAILED(dev->CreateTexture2D(&o, nullptr, &e.out[i])) || FAILED(dev->CreateRenderTargetView(e.out[i], nullptr, &e.rtv[i]))) { LOG("dlss: guide target %d failed", i); return false; }
        IDXGIResource* r = nullptr; HANDLE h = nullptr;
        if (SUCCEEDED(e.out[i]->QueryInterface(__uuidof(IDXGIResource), (void**)&r)) && r) { r->GetSharedHandle(&h); r->Release(); }
        if (!h) { LOG("dlss: guide target %d has no shared handle", i); return false; }
        e.shared[i] = h;
    }
    e.w = dd.Width; e.h = dd.Height; e.depthFmt = dd.Format; e.reset = true;
    return true;
}
void Inv4(const double* m, double* o) {
    double inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    det = fabs(det) > 1e-300 ? 1.0 / det : 0.0;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
}
int g_debug = 0;            // 0 normal, 1 no jitter (DLSS still runs), 2 linear depth read as distance from the eye (radial), 3 zero motion vectors
int g_preset = -1;          // live preset (Page Up), -1 = the ini's
uint32_t g_outW = 0, g_outH = 0;   // upscaling: the output size (0 = DLAA)
const char* PresetName(int p) { switch (p) { case 10: return "J"; case 11: return "K"; case 12: return "L"; case 13: return "M"; case 16: return "NVIDIA recommended"; case 0: return "runtime default"; default: return "?"; } }
bool g_capArmed = false;
float g_lastJit[2] = {};
void (*g_notify)(const char*) = nullptr;
float Half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
    if (e == 0) { float v = ldexpf((float)m, -24); return s ? -v : v; }
    if (e == 31) f = (s << 31) | 0x7F800000 | (m << 13); else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    float v; memcpy(&v, &f, 4); return v;
}
void SaveBmp(const wchar_t* name, int w, int h, const uint8_t* rgb) {   // rgb: w*h*3, top-down
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, name);
    FILE* f = _wfopen(path, L"wb"); if (!f) return;
    int row = (w * 3 + 3) & ~3; uint32_t sz = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' }; memcpy(hd + 2, &sz, 4); uint32_t off = 54; memcpy(hd + 10, &off, 4); uint32_t ih = 40; memcpy(hd + 14, &ih, 4);
    int32_t W = w, H = -h; memcpy(hd + 18, &W, 4); memcpy(hd + 22, &H, 4); hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    uint8_t* line = (uint8_t*)calloc(row, 1);
    for (int y = 0; y < h; ++y) { for (int x = 0; x < w; ++x) { const uint8_t* p = rgb + (y * w + x) * 3; line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0]; } fwrite(line, 1, row, f); }
    free(line); fclose(f);
}
// copies a region of a texture to a CPU buffer (rows packed, bpp bytes per texel)
bool ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* t, int x0, int y0, int w, int h, int bpp, uint8_t* out) {
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = {}; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) return false;
    D3D11_BOX b = { (UINT)x0, (UINT)y0, 0, (UINT)(x0 + w), (UINT)(y0 + h), 1 };
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, &b);
    D3D11_MAPPED_SUBRESOURCE m; bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) { for (int y = 0; y < h; ++y) memcpy(out + (size_t)y * w * bpp, (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * bpp); ctx->Unmap(st, 0); }
    st->Release(); return ok;
}
void Capture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* color, ID3D11Texture2D* out, ID3D11Texture2D* depth, ID3D11Texture2D* mv, int W, int H) {
    int C = 1024; if (C > W) C = W; if (C > H) C = H;
    uint8_t* buf = (uint8_t*)malloc((size_t)W * H * 4 > (size_t)C * C * 4 ? (size_t)W * H * 4 : (size_t)C * C * 4); uint8_t* rgb = (uint8_t*)malloc((size_t)W * H * 3);
    if (!buf || !rgb) { free(buf); free(rgb); return; }
    auto crop = [&](ID3D11Texture2D* t, const wchar_t* name) {   // the centre C x C of t (each at its own size)
        D3D11_TEXTURE2D_DESC td; t->GetDesc(&td);
        if (!ReadBack(dev, ctx, t, ((int)td.Width - C) / 2, ((int)td.Height - C) / 2, C, C, 4, buf)) return;
        for (int i = 0; i < C * C; ++i) { rgb[i * 3] = buf[i * 4]; rgb[i * 3 + 1] = buf[i * 4 + 1]; rgb[i * 3 + 2] = buf[i * 4 + 2]; }
        SaveBmp(name, C, C, rgb);
    };
    crop(color, L"cap_dlss_in.bmp"); crop(out, L"cap_dlss_out.bmp");
    const int w4 = W / 4, h4 = H / 4;
    if (ReadBack(dev, ctx, depth, 0, 0, W, H, 4, buf)) {
        const float* d = (const float*)buf; float lo = 1, hi = 0; double sum = 0;
        for (int i = 0; i < W * H; ++i) { lo = fminf(lo, d[i]); hi = fmaxf(hi, d[i]); sum += d[i]; }
        for (int y = 0; y < h4; ++y) for (int x = 0; x < w4; ++x) {
            float v = d[(y * 4) * W + x * 4]; float t = hi > lo ? (v - lo) / (hi - lo) : 0; t = powf(fminf(fmaxf(t, 0.0f), 1.0f), 8.0f);
            uint8_t g = (uint8_t)(255 - t * 255); uint8_t* p = rgb + (y * w4 + x) * 3; p[0] = p[1] = p[2] = g; }
        SaveBmp(L"cap_dlss_depth.bmp", w4, h4, rgb);
        LOG("dlss capture: depth min %.7f max %.7f mean %.7f", lo, hi, sum / ((double)W * H));
    }
    if (ReadBack(dev, ctx, mv, 0, 0, W, H, 4, buf)) {
        const uint16_t* m = (const uint16_t*)buf; double sx = 0, sy = 0, sa = 0; float mx = 0;
        for (int i = 0; i < W * H; ++i) { float a = Half(m[i * 2]) * W, b = Half(m[i * 2 + 1]) * H; sx += a; sy += b; float l = sqrtf(a * a + b * b); sa += l; mx = fmaxf(mx, l); }
        for (int y = 0; y < h4; ++y) for (int x = 0; x < w4; ++x) {
            int i = (y * 4) * W + x * 4; float a = Half(m[i * 2]) * W, b = Half(m[i * 2 + 1]) * H; uint8_t* p = rgb + (y * w4 + x) * 3;
            p[0] = (uint8_t)fminf(fmaxf(128 + a * 4, 0.0f), 255.0f); p[1] = (uint8_t)fminf(fmaxf(128 + b * 4, 0.0f), 255.0f); p[2] = 128; }
        SaveBmp(L"cap_dlss_mv.bmp", w4, h4, rgb);
        double n = (double)W * H;
        LOG("dlss capture: motion (pixels) mean %.3f %.3f, mean length %.3f, max %.2f; jitter this frame %.3f %.3f (reported %.3f %.3f), debug mode %d",
            sx / n, sy / n, sa / n, mx, g_lastJit[0], g_lastJit[1], g_e[0].jx, g_e[0].jy, g_debug);
    }
    free(buf); free(rgb);
    LOG("dlss capture: saved cap_dlss_in/out (centre 1024x1024), cap_dlss_depth, cap_dlss_mv (1/4 size) in fc2vr_dlss");
}
float Halton(uint32_t i, uint32_t b) { float f = 1, r = 0; while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }
} // namespace

void Configure(const Config& c, const char* binDir) {
    g_c = c; g_on = c.enable != 0;
    MultiByteToWideChar(CP_ACP, 0, binDir, -1, g_dir, MAX_PATH);
    wcscat(g_dir, L"fc2vr_dlss");
    LOG("dlss: %s, %s, preset %s, jitter %d (%d phases), near-pass mask %d", g_on ? "on" : "off", QualityName(c.quality), PresetName(c.preset), c.jitter, c.phases, c.mask);
}
bool Wanted() { return g_on && !g_failed; }
bool Active() { return Wanted() && g_started && g_client.running(); }
void Toggle() {
    if (g_failed) { LOG("dlss: unavailable (see the earlier lines)"); return; }
    g_on = !g_on; g_e[0].reset = g_e[1].reset = true;
    LOG("dlss: toggled %s", g_on ? "on" : "off");
    if (g_notify) g_notify(g_on ? "DLSS on" : "DLSS off");
}
void SetNotify(void (*fn)(const char*)) { g_notify = fn; }
void CycleDebug() {
    g_debug = (g_debug + 1) % 4; g_e[0].reset = g_e[1].reset = true;
    static const char* nm[4] = { "normal", "no jitter", "depth read as radial distance", "no motion vectors" };
    LOG("dlss: debug mode %d (%s)", g_debug, nm[g_debug]);
    char t[96]; snprintf(t, sizeof t, "DLSS mode %d: %s", g_debug, nm[g_debug]); if (g_notify) g_notify(t);
}
void CyclePreset() {
    static const int order[5] = { 11, 10, 13, 12, 16 };
    int cur = g_preset < 0 ? g_c.preset : g_preset, i = 0;
    while (i < 5 && order[i] != cur) ++i;
    g_preset = order[(i + 1) % 5];
    g_e[0].reset = g_e[1].reset = true;
    char t[64]; snprintf(t, sizeof t, "DLSS preset %s", PresetName(g_preset));
    LOG("dlss: %s (the features are rebuilt)", t); if (g_notify) g_notify(t);
}
const char* QualityName(int q) {
    static const char* n[6] = { "DLAA", "Ultra Quality", "Quality", "Balanced", "Performance", "Ultra Performance" };
    return (q >= 0 && q < 6) ? n[q] : "DLAA";
}
float RenderRatio() {
    static const float r[6] = { 1.0f, 0.77f, 0.667f, 0.58f, 0.5f, 0.333f };
    if (!g_c.enable || g_c.quality <= 0 || g_c.quality > 5) return 1.0f;
    return r[g_c.quality];
}
void SetOutputSize(uint32_t w, uint32_t h) { g_outW = w; g_outH = h; LOG("dlss: output %ux%u (%s)", w, h, w ? QualityName(g_c.quality) : "DLAA"); }
void ArmCapture() { if (Active()) { g_capArmed = true; LOG("dlss: capture armed"); } }
void BeginFrame(float* sx, float* sy) {
    *sx = *sy = 0;
    if (!Wanted() || !g_c.jitter || !g_started || g_debug == 1) return;   // jitter only once the helper is up (not in the no-jitter test mode)
    uint32_t n = g_c.phases > 1 ? (uint32_t)g_c.phases : 16;
    if (g_outW) {   // upscaling: NVIDIA's guide, 8 x (output / render)^2 phases
        float rr = RenderRatio(); uint32_t m = (uint32_t)ceilf(8.0f / (rr * rr));
        if (m > n) n = m;
    }
    uint32_t k = (g_phase++ % n) + 1;
    *sx = Halton(k, 2) - 0.5f; *sy = Halton(k, 3) - 0.5f;
    g_lastJit[0] = *sx; g_lastJit[1] = *sy;
}

void SetLinearDepth(ID3D10Texture2D* t) {
    if (t == g_lin) return;
    Rel(g_linSrv); if (g_lin) g_lin->Release();
    g_lin = t; g_linScale = -1;
    if (g_lin) g_lin->AddRef();
}
void PrepareEye10(ID3D10Device* dev, int ei, ID3D10Texture2D* depth, const double vpCur[16], const double* vpPrev,
                  float nearMaxDepth, float jitterX, float jitterY, float camNear, float camFar, const float tan4[4]) {
    if (ei < 0 || ei > 1) return;
    Eye10& e = g_e[ei];
    e.fresh = false;
    if (!Wanted() || !depth || !dev) return;
    if (!Init10(dev)) { g_failed = true; return; }
    if (!Ensure10(dev, e, depth)) { g_failed = true; Release10(e); return; }
    CB cb = {};
    if (vpPrev && g_debug != 3) {
        double inv[16], m[16];
        Inv4(vpCur, inv);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { double s = 0; for (int k = 0; k < 4; ++k) s += inv[r * 4 + k] * vpPrev[k * 4 + c]; m[r * 4 + c] = s; }
        for (int i = 0; i < 16; ++i) cb.M[i] = (float)m[i];
        cb.hist = 1;
    } else { cb.M[0] = cb.M[5] = cb.M[10] = cb.M[15] = 1; cb.hist = 0; }
    cb.size[0] = (float)e.w; cb.size[1] = (float)e.h;
    cb.nearMax = nearMaxDepth; cb.useMask = g_c.mask ? 1.0f : 0.0f;
    cb.jit[0] = jitterX; cb.jit[1] = jitterY;
    if (g_lin && !g_linSrv) {
        D3D10_TEXTURE2D_DESC ld; g_lin->GetDesc(&ld);
        if (ld.Width == e.w && ld.Height == e.h && (ld.BindFlags & D3D10_BIND_SHADER_RESOURCE)) {
            D3D10_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = ld.Format; sv.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            if (FAILED(dev->CreateShaderResourceView(g_lin, &sv, &g_linSrv))) g_linSrv = nullptr;
        }
        LOG("dlss: linear depth target %p (%ux%u fmt %d) %s", (void*)g_lin, ld.Width, ld.Height, (int)ld.Format, g_linSrv ? "in use" : "not usable");
    }
    if (g_linSrv) {
        const float n = camNear > 0 ? camNear : 0.1f, f = camFar > n ? camFar : 1000.0f;
        cb.linMode = 1; cb.P10 = f / (n - f); cb.P14 = n * f / (n - f);
        for (int k = 0; k < 4; ++k) cb.tanR[k] = tan4[k];
        cb.radial = g_debug == 2 ? 1.0f : 0.0f; cb.camNear = n;
        cb.linScale = g_linScale > 0 ? g_linScale : f;   // until decided: assume it holds z / far
    }
    g_sb->Capture();
    dev->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D10ShaderResourceView* none[2] = {};
    dev->PSSetShaderResources(0, 2, none);
    dev->CopyResource(e.depthCopy, depth);
    {   // is there depth in it? (checked for a while, then trusted)
        static int checks = 0; static uint32_t n = 0;
        if (ei == 0 && checks < 8 && (n++ % 120) == 0) {
            ++checks; float lo = 0, hi = 0, slo = 0, shi = 0;
            bool a = DepthProbe(dev, e.depthCopy, &lo, &hi);
            LOG("dlss: depth check %d: depth buffer copy %s %.6f..%.6f", checks, a ? "ok" : "unreadable", lo, hi);
            if (g_lin) {
                float clo = 0, chi = 0;
                bool b = DepthProbe(dev, g_lin, &slo, &shi, true), c = DepthProbe(dev, g_lin, &clo, &chi);
                LOG("dlss: depth check %d: linear depth target %s whole image %.4f..%.4f, centre %.4f..%.4f (camera near %.3f far %.1f)",
                    checks, b && c ? "ok" : "unreadable", slo, shi, clo, chi, camNear, camFar);
                if (b && shi > 0) {
                    float sc = shi <= 1.001f ? camFar : 1.0f;
                    if (sc != g_linScale) { g_linScale = sc; LOG("dlss: linear depth read as %s", shi <= 1.001f ? "z / far (0..1)" : "metres"); }
                }
            }
        }
    }
    dev->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    dev->OMSetRenderTargets(3, e.rtv, nullptr);
    D3D10_VIEWPORT vp = { 0, 0, e.w, e.h, 0.0f, 1.0f };
    dev->RSSetViewports(1, &vp);
    dev->RSSetState(g_rs);
    dev->OMSetDepthStencilState(g_dss, 0);
    float bf[4] = {};
    dev->OMSetBlendState(g_bs, bf, 0xffffffff);
    dev->IASetInputLayout(nullptr);
    dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev->VSSetShader(g_vs); dev->GSSetShader(nullptr); dev->PSSetShader(g_ps);
    dev->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D10ShaderResourceView* srvs[2] = { e.depthSrv, g_linSrv };
    dev->PSSetShaderResources(0, 2, srvs);
    dev->Draw(3, 0);
    dev->PSSetShaderResources(0, 2, none);
    dev->OMSetRenderTargets(0, nullptr, nullptr);
    g_sb->Apply();
    e.fresh = true;
    if (!vpPrev) e.reset = true;
    // DLSS reads the offset as where the content moved (the Dishonored VR helper tests proved the negation)
    const float sg = -1.0f;
    e.jx = sg * jitterX; e.jy = sg * jitterY;
}

ID3D11Texture2D* Evaluate11(ID3D11Device* dev, ID3D11DeviceContext* ctx, int ei, ID3D11Texture2D* color) {
    if (ei < 0 || ei > 1 || !Wanted() || !color) return nullptr;
    Eye10& e = g_e[ei];
    if (!e.fresh) return nullptr;
    e.fresh = false;
    char why[256] = "";
    if (!g_started) {
        wchar_t exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%ls\\fc2vr_dlss_host64.exe", g_dir);
        fc2vr::dlss::StartParams sp; sp.hostExe = exe; sp.dataDir = g_dir;
        sp.log = [](int lvl, const char* line) { LOG("%s%s", lvl == 2 ? "ERROR " : lvl == 1 ? "warning " : "", line); };
        if (!g_client.start(dev, sp, why, sizeof why)) { LOG("dlss: the helper did not start: %s - DLSS off", why); g_failed = true; return nullptr; }
        g_started = true;
    }
    if (!g_client.running()) { if (!g_failed) LOG("dlss: the helper stopped - DLSS off"); g_failed = true; return nullptr; }
    for (int i = 0; i < 3; ++i) if (!e.out11[i]) {
        if (FAILED(dev->OpenSharedResource(e.shared[i], __uuidof(ID3D11Texture2D), (void**)&e.out11[i]))) { LOG("dlss: guide %d not openable on D3D11 - DLSS off", i); g_failed = true; return nullptr; }
    }
    D3D11_TEXTURE2D_DESC cd; color->GetDesc(&cd);
    if (cd.Width != e.w || cd.Height != e.h) { static int n = 0; if (n++ < 3) LOG("dlss: colour %ux%u differs from depth %ux%u", cd.Width, cd.Height, e.w, e.h); return nullptr; }
    static int builtPreset[2] = { -1, -1 };
    const int want = g_preset < 0 ? g_c.preset : g_preset;
    const uint32_t ow = g_outW ? g_outW : e.w, oh = g_outH ? g_outH : e.h;
    if (!g_client.built(ei, e.w, e.h, ow, oh, cd.Format) || builtPreset[ei] != want) {
        builtPreset[ei] = want;
        if (!g_client.build(ei, e.w, e.h, ow, oh, cd.Format, want, why, sizeof why, false, g_c.autoExposure != 0)) {
            LOG("dlss: eye %d build failed: %s - DLSS off", ei, why); g_failed = true; return nullptr;
        }
        e.reset = true;
    }
    fc2vr::dlss::EyeInputs in;
    in.color = color; in.depth = e.out11[0]; in.motion = e.out11[1]; in.bias = g_c.mask ? e.out11[2] : nullptr;
    in.reset = e.reset; in.jitterX = e.jx; in.jitterY = e.jy; in.mvSign = 1.0f; in.sharpness = g_c.sharpness;
    if (!g_client.evaluate(ctx, ei, in, why, sizeof why)) {
        static int n = 0; if (n++ < 10) LOG("dlss: eye %d evaluate: %s", ei, why);
        if (!g_client.running()) g_failed = true;
        return nullptr;
    }
    e.reset = false;
    if (g_capArmed && ei == 0) { g_capArmed = false; Capture(dev, ctx, color, g_client.output_texture(ei), e.out11[0], e.out11[1], (int)e.w, (int)e.h); }
    static uint64_t frames = 0;
    if ((++frames % 1800) == 0) {
        auto& st = g_client.stats;
        LOG("dlss: %llu eye images; per eye CPU %.2f / %.2f ms, helper GPU %.2f / %.2f ms, refused %llu / %llu",
            (unsigned long long)frames, st.frames[0] ? st.cpuMsSum[0] / st.frames[0] : 0.0, st.frames[1] ? st.cpuMsSum[1] / st.frames[1] : 0.0,
            st.gpuN[0] ? st.gpuMsSum[0] / st.gpuN[0] : -1.0, st.gpuN[1] ? st.gpuMsSum[1] / st.gpuN[1] : -1.0,
            (unsigned long long)st.refused[0], (unsigned long long)st.refused[1]);
    }
    return g_client.output_texture(ei);
}
void Shutdown() { g_client.stop(); for (auto& e : g_e) Release10(e); }
}
