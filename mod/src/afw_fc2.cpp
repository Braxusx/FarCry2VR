// afw_fc2.cpp - see afw_fc2.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "afw_fc2.h"
#include "log.h"

namespace afw {
namespace {
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
Config g_c;
bool g_on = false, g_failed = false, g_init = false;
int g_debug = 0;
uint32_t g_epoch = 1;

const char kHlsl[] = R"(
Texture2D<float4> gColor : register(t0);
Texture2D<float> gLin : register(t1);
SamplerState gLinS : register(s0);
cbuffer C : register(b0) {
    row_major float4x4 M;   // source clip -> target clip (row vectors)
    float2 srcSize; float P10; float P14;
    float handDepth; float handMin; float mode; float sheet;
    float step; float cellsX; float dbg; float gripMask;   // gripMask: bit 0 right, bit 1 left valid
    float4 hr; float4 hl;   // per hand: its grip in the source image (ndc xy) and its move to the target (ndc zw)
    float4 rr; float4 rl;   // per hand: on-screen turn and scale about the grip (xy = s*cos, s*sin, in pixels)
};
struct MOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float hand : TEXCOORD1; };
static const uint2 kOff[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };
MOut vsmesh(uint id : SV_VertexID) {
    uint cell = id / 6, k = id % 6;
    uint cx = cell % (uint)cellsX, cy = cell / (uint)cellsX;
    uint2 tex = min(uint2((cx + kOff[k].x) * (uint)step, (cy + kOff[k].y) * (uint)step), uint2(srcSize) - 1);
    float2 uv = (float2(tex) + 0.5) / srcSize;
    float z = gLin.Load(int3(tex, 0));
    bool hand = z > 0 && z < handDepth;
    float zz = hand ? max(z, handMin) : z;
    float d = zz > 0 ? saturate(-P10 + P14 / zz) : 1;
    float4 q = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1), M);
    if (q.w > 0) q.z = clamp(q.z, 0, q.w * 0.99999);
    if (mode > 0.5 && mode < 1.5 && hand && gripMask > 0.5) {   // the hands follow the controllers from the source image to now
        uint gm = (uint)gripMask;
        float2 n = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        float dR = (gm & 1) ? length((n - hr.xy) * srcSize) : 1e9, dL = (gm & 2) ? length((n - hl.xy) * srcSize) : 1e9;
        float4 h = dL < dR ? hl : hr; float2 r = dL < dR ? rl.xy : rr.xy;
        float2 hp = srcSize * 0.5;
        float2 p = (n - h.xy) * hp;
        float2 rp = float2(r.x * p.x - r.y * p.y, r.y * p.x + r.x * p.y);
        float2 nn = h.xy + h.zw + rp / hp;
        q.xy += (nn - n) * q.w;
    }   // the sky sits at the far plane: never let it be clipped
    if (mode > 0.5 && mode < 1.5) q.z *= 0.001;   // the hands are drawn over the world, as the game draws them
    MOut o; o.pos = q; o.uv = uv; o.hand = hand ? 1 : 0; return o;
}
float3 Tint(float3 c) {
#ifdef AFW_TINT
    c = mode < 0.5 ? c : mode < 1.5 ? lerp(c, float3(0, 1, 0), 0.35) : lerp(c, float3(0, 0.3, 1), 0.35);
#endif
    return c;
}
float4 psmesh(MOut i) : SV_Target {
    float2 a = ddx(i.uv) * srcSize, b = ddy(i.uv) * srcSize;
    float area = abs(a.x * b.y - a.y * b.x);
    bool handPass = mode > 0.5 && mode < 1.5;
    if (handPass ? i.hand < 0.99 : i.hand > 0.01) discard;
    if (!handPass && area < sheet) discard;   // a sheet stretched across a depth break
    float3 c = gColor.SampleLevel(gLinS, i.uv, 0).rgb;
    return float4(Tint(c), 1);
}
Texture2D<float4> gWarp : register(t0);
Texture2D<float> gWarpD : register(t1);
struct FOut { float4 pos : SV_Position; };
FOut vsfull(uint id : SV_VertexID) { float2 uv = float2((id << 1) & 2, id & 2); FOut o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 psfill(FOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float4 c = gWarp.Load(int3(p, 0));
    if (c.a > 0.5) return float4(c.rgb, 1);
    // nothing shows this point: extend the farthest covered neighbour along the row (the background)
    float bd = -1; float3 best = float3(0, 0, 0);
    int w = (int)srcSize.x;
    [unroll] for (int s = 4; s <= 128; s *= 2) {
        [unroll] for (int sg = -1; sg <= 1; sg += 2) {
            int2 q = int2(clamp(p.x + sg * s, 0, w - 1), p.y);
            float4 cc = gWarp.Load(int3(q, 0));
            if (cc.a > 0.5) { float dd = gWarpD.Load(int3(q, 0)); if (dd > bd) { bd = dd; best = cc.rgb; } }
        }
    }
#ifdef AFW_TINT
    best = lerp(best, float3(1, 0, 1), 0.5);
#endif
    return float4(best, 1);
}
)";
struct CB { float M[16]; float srcSize[2]; float P10, P14; float handDepth, handMin, mode, sheet; float step, cellsX, dbg, gripMask; float hr[4], hl[4]; float rr[4], rl[4]; };
static_assert(sizeof(CB) == 176, "CB layout");

struct Src {
    ID3D10Texture2D* color = nullptr; ID3D10ShaderResourceView* colorSrv = nullptr;
    ID3D10Texture2D* lin = nullptr; ID3D10ShaderResourceView* linSrv = nullptr;
    double vp[16] = {}; float P10 = 0, P14 = 0; uint32_t frame = 0, epoch = 0; bool have = false; float grip[2][6] = {};
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
Src g_src[2];
ID3D10VertexShader *g_vsMesh = nullptr, *g_vsFull = nullptr; ID3D10PixelShader *g_psMesh = nullptr, *g_psFill = nullptr, *g_psMeshT = nullptr, *g_psFillT = nullptr;
ID3D10Buffer* g_cb = nullptr; ID3D10SamplerState* g_samp = nullptr;
ID3D10DepthStencilState *g_dssWrite = nullptr, *g_dssFill = nullptr, *g_dssOff = nullptr;
ID3D10RasterizerState* g_rs = nullptr; ID3D10BlendState* g_bs = nullptr; ID3D10StateBlock* g_sb = nullptr;
ID3D10Texture2D *g_warp = nullptr, *g_out = nullptr, *g_ds = nullptr;
ID3D10RenderTargetView *g_warpRtv = nullptr, *g_outRtv = nullptr; ID3D10ShaderResourceView *g_warpSrv = nullptr, *g_dsSrv = nullptr;
ID3D10DepthStencilView* g_dsv = nullptr;
UINT g_tw = 0, g_th = 0;
uint64_t g_rebuilds = 0, g_refused = 0;

bool Init(ID3D10Device* dev) {
    if (g_init) return true;
    if (g_failed) return false;
    g_failed = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!comp) { LOG("afw: d3dcompiler_47.dll not available - AFW off"); return false; }
    auto build = [&](const char* entry, const char* target, ID3DBlob** out, bool tint = false) -> bool {
        ID3DBlob* err = nullptr;
        D3D_SHADER_MACRO mac[2] = { { "AFW_TINT", "1" }, { nullptr, nullptr } };
        HRESULT hr = comp(kHlsl, sizeof(kHlsl) - 1, "fc2vr_afw", tint ? mac : nullptr, nullptr, entry, target, 0, 0, out, &err);
        if (FAILED(hr)) { LOG("afw: %s compile failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?"); Rel(err); return false; }
        Rel(err); return true;
    };
    ID3DBlob *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    bool ok = build("vsmesh", "vs_4_0", &a) && build("psmesh", "ps_4_0", &b) && build("vsfull", "vs_4_0", &c) && build("psfill", "ps_4_0", &d);
    ok = ok && SUCCEEDED(dev->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), &g_vsMesh)) &&
         SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), &g_psMesh)) &&
         SUCCEEDED(dev->CreateVertexShader(c->GetBufferPointer(), c->GetBufferSize(), &g_vsFull)) &&
         SUCCEEDED(dev->CreatePixelShader(d->GetBufferPointer(), d->GetBufferSize(), &g_psFill));
    Rel(a); Rel(b); Rel(c); Rel(d);
    ID3DBlob *bt = nullptr, *dt = nullptr;   // the debug-tint variants
    ok = ok && build("psmesh", "ps_4_0", &bt, true) && build("psfill", "ps_4_0", &dt, true) &&
         SUCCEEDED(dev->CreatePixelShader(bt->GetBufferPointer(), bt->GetBufferSize(), &g_psMeshT)) &&
         SUCCEEDED(dev->CreatePixelShader(dt->GetBufferPointer(), dt->GetBufferSize(), &g_psFillT));
    Rel(bt); Rel(dt);
    D3D10_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB); bd.Usage = D3D10_USAGE_DEFAULT; bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    D3D10_SAMPLER_DESC sd = {}; sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = 0;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
    D3D10_DEPTH_STENCIL_DESC w = {};
    w.DepthEnable = TRUE; w.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL; w.DepthFunc = D3D10_COMPARISON_LESS;
    w.StencilEnable = TRUE; w.StencilReadMask = w.StencilWriteMask = 0xFF;
    w.FrontFace.StencilFunc = D3D10_COMPARISON_ALWAYS; w.FrontFace.StencilPassOp = D3D10_STENCIL_OP_REPLACE;
    w.FrontFace.StencilFailOp = w.FrontFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP; w.BackFace = w.FrontFace;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&w, &g_dssWrite));
    D3D10_DEPTH_STENCIL_DESC f = w;   // only where nothing was drawn yet
    f.FrontFace.StencilFunc = D3D10_COMPARISON_EQUAL; f.FrontFace.StencilPassOp = D3D10_STENCIL_OP_KEEP; f.BackFace = f.FrontFace;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&f, &g_dssFill));
    D3D10_DEPTH_STENCIL_DESC o = {}; o.DepthEnable = FALSE; o.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&o, &g_dssOff));
    D3D10_RASTERIZER_DESC rd = {}; rd.FillMode = D3D10_FILL_SOLID; rd.CullMode = D3D10_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    D3D10_BLEND_DESC bl = {}; for (int i = 0; i < 8; ++i) bl.RenderTargetWriteMask[i] = D3D10_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_bs));
    typedef HRESULT (WINAPI* CreateSB_t)(ID3D10Device*, D3D10_STATE_BLOCK_MASK*, ID3D10StateBlock**);
    typedef HRESULT (WINAPI* MaskAll_t)(D3D10_STATE_BLOCK_MASK*);
    HMODULE d10 = GetModuleHandleW(L"d3d10.dll"); if (!d10) d10 = LoadLibraryW(L"d3d10.dll");
    CreateSB_t csb = d10 ? (CreateSB_t)GetProcAddress(d10, "D3D10CreateStateBlock") : nullptr;
    MaskAll_t mall = d10 ? (MaskAll_t)GetProcAddress(d10, "D3D10StateBlockMaskEnableAll") : nullptr;
    if (ok && csb && mall) { D3D10_STATE_BLOCK_MASK m; mall(&m); ok = SUCCEEDED(csb(dev, &m, &g_sb)); } else ok = false;
    if (!ok) { LOG("afw: setup failed - AFW off"); return false; }
    g_failed = false; g_init = true;
    LOG("afw: rebuild passes ready (grid step %d, hands nearer than %.2f m)", g_c.gridStep, g_c.handDepth);
    return true;
}
void ReleaseTargets() { Rel(g_warpRtv); Rel(g_outRtv); Rel(g_warpSrv); Rel(g_dsSrv); Rel(g_dsv); Rel(g_warp); Rel(g_out); Rel(g_ds); g_tw = g_th = 0; }
bool EnsureTargets(ID3D10Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_tw == w && g_th == h && g_warp) return true;
    ReleaseTargets();
    D3D10_TEXTURE2D_DESC t = {}; t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1; t.Format = fmt; t.SampleDesc.Count = 1;
    t.Usage = D3D10_USAGE_DEFAULT; t.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g_warp)) && SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g_out)) &&
              SUCCEEDED(dev->CreateRenderTargetView(g_warp, nullptr, &g_warpRtv)) && SUCCEEDED(dev->CreateRenderTargetView(g_out, nullptr, &g_outRtv)) &&
              SUCCEEDED(dev->CreateShaderResourceView(g_warp, nullptr, &g_warpSrv));
    D3D10_TEXTURE2D_DESC d = t; d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.BindFlags = D3D10_BIND_DEPTH_STENCIL | D3D10_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_ds));
    D3D10_DEPTH_STENCIL_VIEW_DESC dv = {}; dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D10_DSV_DIMENSION_TEXTURE2D;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilView(g_ds, &dv, &g_dsv));
    D3D10_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sv.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    ok = ok && SUCCEEDED(dev->CreateShaderResourceView(g_ds, &sv, &g_dsSrv));
    if (!ok) { LOG("afw: rebuild targets %ux%u failed - AFW off", w, h); ReleaseTargets(); g_failed = true; return false; }
    g_tw = w; g_th = h;
    return true;
}
void ReleaseSrc(Src& s) { Rel(s.colorSrv); Rel(s.color); Rel(s.linSrv); Rel(s.lin); s = Src{}; }
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
// source clip -> target clip: inverse(source view-projection) * target view-projection (row vectors)
void SrcToTarget(const double* vpSrc, const double* vpDst, float* out) {
    double inv[16]; Inv4(vpSrc, inv);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { double s = 0; for (int k = 0; k < 4; ++k) s += inv[r * 4 + k] * vpDst[k * 4 + c]; out[r * 4 + c] = (float)s; }
}
} // namespace

void Configure(const Config& c) {
    g_c = c; g_on = c.enable != 0; g_debug = c.debug;
    if (g_c.gridStep < 1) g_c.gridStep = 1; if (g_c.gridStep > 8) g_c.gridStep = 8;
    LOG("afw: %s (experimental), grid step %d, hand depth %.2f m (min %.2f m)", g_on ? "on" : "off", g_c.gridStep, g_c.handDepth, g_c.handMin);
}
bool Wanted() { return g_on && !g_failed; }
void Invalidate() { ++g_epoch; }
void Toggle() { if (g_failed) { LOG("afw: unavailable (see the earlier lines)"); return; } g_on = !g_on; Invalidate(); LOG("afw: toggled %s", g_on ? "on" : "off"); }
void CycleDebug() { g_debug = !g_debug; LOG("afw: debug tint %s", g_debug ? "on" : "off"); }

void Store(ID3D10Device* dev, int e, ID3D10Texture2D* color, ID3D10Texture2D* lin, const double vp[16], float n, float f, uint32_t frame, const float grips[2][6]) {
    if (e < 0 || e > 1 || !Wanted() || !color || !lin || !dev) return;
    if (!Init(dev)) return;
    Src& s = g_src[e];
    D3D10_TEXTURE2D_DESC cd, ld; color->GetDesc(&cd); lin->GetDesc(&ld);
    if (cd.Width != ld.Width || cd.Height != ld.Height) { static int n0 = 0; if (n0++ < 3) LOG("afw: colour %ux%u and depth %ux%u differ", cd.Width, cd.Height, ld.Width, ld.Height); return; }
    if (!s.color || s.w != cd.Width || s.h != cd.Height || s.fmt != cd.Format) {
        ReleaseSrc(s);
        D3D10_TEXTURE2D_DESC t = cd; t.BindFlags = D3D10_BIND_SHADER_RESOURCE; t.MiscFlags = 0; t.CPUAccessFlags = 0; t.Usage = D3D10_USAGE_DEFAULT; t.MipLevels = 1; t.ArraySize = 1;
        D3D10_TEXTURE2D_DESC u = ld; u.BindFlags = D3D10_BIND_SHADER_RESOURCE; u.MiscFlags = 0; u.CPUAccessFlags = 0; u.Usage = D3D10_USAGE_DEFAULT; u.MipLevels = 1; u.ArraySize = 1;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &s.color)) && SUCCEEDED(dev->CreateShaderResourceView(s.color, nullptr, &s.colorSrv)) &&
                  SUCCEEDED(dev->CreateTexture2D(&u, nullptr, &s.lin)) && SUCCEEDED(dev->CreateShaderResourceView(s.lin, nullptr, &s.linSrv));
        if (!ok) { LOG("afw: source textures for eye %d failed - AFW off", e); ReleaseSrc(s); g_failed = true; return; }
        s.w = cd.Width; s.h = cd.Height; s.fmt = cd.Format;
        LOG("afw: eye %d source %ux%u colour fmt %d, depth fmt %d", e, cd.Width, cd.Height, (int)cd.Format, (int)ld.Format);
    }
    dev->CopyResource(s.color, color);
    dev->CopyResource(s.lin, lin);
    {   // what the depth image holds for the hands and weapon (bottom half of the image, nearer than 2 m) - a few times
        static int probes = 0; static uint32_t last = 0;
        if (probes < 4 && frame - last > 450) {
            last = frame; ++probes;
            D3D10_TEXTURE2D_DESC sd = ld; sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D10_USAGE_STAGING; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
            ID3D10Texture2D* st = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
                dev->CopyResource(st, s.lin);
                D3D10_MAPPED_TEXTURE2D m;
                if (SUCCEEDED(st->Map(0, D3D10_MAP_READ, 0, &m))) {
                    int bins[8] = {}; float lo = 1e9f, hi = 0; double sum = 0; int n = 0;
                    for (UINT y = ld.Height / 2; y < ld.Height; y += 4) for (UINT x = 0; x < ld.Width; x += 4) {
                        float v = *(const float*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch + x * 4);
                        if (v > 0 && v < 2.0f) { ++n; sum += v; lo = fminf(lo, v); hi = fmaxf(hi, v); int b = (int)(v / 0.25f); bins[b < 8 ? b : 7]++; }
                    }
                    st->Unmap(0);
                    LOG("afw: eye %d depth nearer than 2 m in the lower half: %d samples, %.3f..%.3f m, mean %.3f; 0.25 m bins %d %d %d %d %d %d %d %d",
                        e, n, n ? lo : 0.0f, hi, n ? sum / n : 0.0, bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7]);
                }
                st->Release();
            }
        }
    }
    memcpy(s.vp, vp, sizeof s.vp);
    const float nn = n > 0 ? n : 0.25f, ff = f > nn ? f : 1024.0f;
    s.P10 = ff / (nn - ff); s.P14 = nn * ff / (nn - ff);
    s.frame = frame; s.epoch = g_epoch; s.have = true;
    if (grips) memcpy(s.grip, grips, sizeof s.grip); else for (auto& g : s.grip) for (float& v : g) v = NAN;
}
bool HasSource(int e, uint32_t frame) {
    if (e < 0 || e > 1 || !Wanted()) return false;
    const Src& s = g_src[e];
    return s.have && s.epoch == g_epoch && frame - s.frame <= 2;
}
static ID3D10ShaderResourceView* g_ovSrv[2] = {}; static ID3D10Texture2D* g_ovFor[2] = {};
// a game-space point through a view-projection (row vectors) to ndc; false behind the eye
static bool Ndc(const double* vp, const float* p, float* o) {
    double c[4]; for (int k = 0; k < 4; ++k) c[k] = p[0] * vp[k] + p[1] * vp[4 + k] + p[2] * vp[8 + k] + vp[12 + k];
    if (!(c[3] > 1e-6)) return false;
    o[0] = (float)(c[0] / c[3]); o[1] = (float)(c[1] / c[3]); return true;
}
ID3D10Texture2D* Rebuild(ID3D10Device* dev, int held, int fresh, const double vpTarget[16], ID3D10Texture2D* heldColor, const float gripsNow[2][6]) {
    if (!Wanted() || held < 0 || held > 1 || fresh != 1 - held || !dev || !g_init) return nullptr;
    Src& H = g_src[held]; Src& F = g_src[fresh];
    if (!H.have || !F.have || H.epoch != g_epoch || F.epoch != g_epoch || H.w != F.w || H.h != F.h) { ++g_refused; return nullptr; }
    if (!EnsureTargets(dev, H.w, H.h, H.fmt)) return nullptr;
    const UINT step = (UINT)g_c.gridStep, cellsX = H.w / step, cellsY = H.h / step;
    CB cb = {};
    cb.srcSize[0] = (float)H.w; cb.srcSize[1] = (float)H.h;
    cb.handDepth = g_c.handDepth; cb.handMin = g_c.handMin; cb.sheet = 1.0f / 9.0f;
    cb.step = (float)step; cb.cellsX = (float)cellsX; cb.dbg = g_debug ? 1.0f : 0.0f;
    {   // each hand's grip: where it was in the held image and how far it moved on screen since (head and hand motion)
        uint32_t gm = 0;
        for (int k = 0; k < 2 && gripsNow && g_c.handMotion; ++k) {
            const float* go = H.grip[k == 0 ? 1 : 0]; const float* gn = gripsNow[k == 0 ? 1 : 0];   // k 0 = right, 1 = left
            float o[2], n[2];
            if (go[0] == go[0] && gn[0] == gn[0] && Ndc(H.vp, go, o) && Ndc(vpTarget, gn, n)) {
                float dx = n[0] - o[0], dy = n[1] - o[1];
                if (fabsf(dx) < 0.3f && fabsf(dy) < 0.3f) {
                    float* hv = k == 0 ? cb.hr : cb.hl; hv[0] = o[0]; hv[1] = o[1]; hv[2] = dx; hv[3] = dy; gm |= 1u << k;
                    float* rv = k == 0 ? cb.rr : cb.rl; rv[0] = 1; rv[1] = 0;
                    // the turn: the line from the grip to a point ahead of the controller, before and now (pixels)
                    float o2[2], n2[2];
                    if (g_c.handMotion >= 2 && go[3] == go[3] && gn[3] == gn[3] && Ndc(H.vp, go + 3, o2) && Ndc(vpTarget, gn + 3, n2)) {
                        float ax = (o2[0] - o[0]) * H.w, ay = (o2[1] - o[1]) * H.h, bx = (n2[0] - n[0]) * H.w, by = (n2[1] - n[1]) * H.h;
                        float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
                        if (la > 40.0f && lb > 40.0f) {   // the line must be long enough on screen to give a stable angle
                            float c = (ax * bx + ay * by) / (la * lb), sn = (ax * by - ay * bx) / (la * lb), sc = lb / la;
                            if (c > 0.85f && sc > 0.8f && sc < 1.25f) { rv[0] = sc * c; rv[1] = sc * sn; }
                        }
                    }
                }
            }
        }
        cb.gripMask = (float)gm;
    }
    g_sb->Capture();
    ID3D10ShaderResourceView* none[2] = {};
    dev->PSSetShaderResources(0, 2, none); dev->VSSetShaderResources(0, 2, none);
    float clr[4] = { 0, 0, 0, 0 };
    dev->OMSetRenderTargets(1, &g_warpRtv, g_dsv);
    dev->ClearRenderTargetView(g_warpRtv, clr);
    dev->ClearDepthStencilView(g_dsv, D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1.0f, 0);
    D3D10_VIEWPORT vp = { 0, 0, H.w, H.h, 0.0f, 1.0f };
    dev->RSSetViewports(1, &vp); dev->RSSetState(g_rs);
    float bf[4] = {}; dev->OMSetBlendState(g_bs, bf, 0xffffffff);
    dev->IASetInputLayout(nullptr); dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev->GSSetShader(nullptr);
    dev->VSSetShader(g_vsMesh); dev->PSSetShader(g_debug ? g_psMeshT : g_psMesh);
    dev->VSSetConstantBuffers(0, 1, &g_cb); dev->PSSetConstantBuffers(0, 1, &g_cb);
    dev->PSSetSamplers(0, 1, &g_samp);
    const UINT verts = cellsX * cellsY * 6;
    auto pass = [&](Src& s, float mode, ID3D10DepthStencilState* dss, UINT ref, bool screenFixed = false) {
        if (screenFixed) { memset(cb.M, 0, sizeof cb.M); cb.M[0] = cb.M[5] = cb.M[10] = cb.M[15] = 1; }
        else SrcToTarget(s.vp, vpTarget, cb.M); cb.P10 = s.P10; cb.P14 = s.P14; cb.mode = mode;
        dev->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
        ID3D10ShaderResourceView* sr[2] = { s.colorSrv, s.linSrv };
        dev->VSSetShaderResources(0, 2, sr); dev->PSSetShaderResources(0, 2, sr);
        dev->OMSetDepthStencilState(dss, ref);
        dev->Draw(verts, 0);
    };
    // the held eye's colour: what it showed last time (the DLSS result when DLSS runs), else its stored image
    ID3D10ShaderResourceView* heldSrv = H.colorSrv;
    if (heldColor) {
        if (g_ovFor[held] != heldColor) { Rel(g_ovSrv[held]); g_ovFor[held] = heldColor; if (FAILED(dev->CreateShaderResourceView(heldColor, nullptr, &g_ovSrv[held]))) g_ovSrv[held] = nullptr; }
        if (g_ovSrv[held]) heldSrv = g_ovSrv[held];
    }
    ID3D10ShaderResourceView* keep = H.colorSrv; H.colorSrv = heldSrv;
    pass(H, 0.0f, g_dssWrite, 1);    // the held eye's own world, moved to its new view
    // The hands and weapon. FC2 stores them in the depth image much nearer than they are (0.1-0.25 m for hands
    // at arm's length: the viewmodel is drawn shrunk toward the eye), so moving them to the other eye by depth
    // puts them at the wrong distance (doubled). AfwHands 0: the held eye's own hands from its last image,
    // fixed to the view (they move with the head; one frame of lag for hand motion). 1: from the fresh eye.
    if (g_c.hands == 1) pass(F, 1.0f, g_dssWrite, 1);
    else pass(H, 1.0f, g_dssWrite, 1, true);
    H.colorSrv = keep;
    pass(F, 2.0f, g_dssFill, 0);     // the world the held image could not see, from the fresh eye
    dev->VSSetShaderResources(0, 2, none); dev->PSSetShaderResources(0, 2, none);
    // fill what neither source shows
    dev->OMSetRenderTargets(1, &g_outRtv, nullptr);
    dev->OMSetDepthStencilState(g_dssOff, 0);
    dev->VSSetShader(g_vsFull); dev->PSSetShader(g_debug ? g_psFillT : g_psFill);
    dev->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ID3D10ShaderResourceView* fs[2] = { g_warpSrv, g_dsSrv };
    dev->PSSetShaderResources(0, 2, fs);
    dev->Draw(3, 0);
    dev->PSSetShaderResources(0, 2, none);
    dev->OMSetRenderTargets(0, nullptr, nullptr);
    g_sb->Apply();
    if ((++g_rebuilds % 2700) == 1) LOG("afw: %llu rebuilds, %llu refused (grid %ux%u x %u texels, %u vertices per pass), debug tint %s",
                                        (unsigned long long)g_rebuilds, (unsigned long long)g_refused, cellsX, cellsY, step, verts, g_debug ? "ON" : "off");
    return g_out;
}
}
