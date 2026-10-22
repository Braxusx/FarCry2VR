// perf.cpp - see perf.h
#include "perf.h"
#include "log.h"
#include "gameaddr.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>

void LogWrite(const char* fmt, ...);

namespace perf {

// ---------------------------------------------------------------- GPU timestamps
static ID3D10Device* g_dev = nullptr;
struct FrameQ { ID3D10Query* disj = nullptr; ID3D10Query* ts[G_COUNT]{}; bool issued[G_COUNT]{}; bool pending = false; bool open = false; };
static const int kRing = 8;
static FrameQ g_ring[kRing];
static int g_cur = -1;
static double g_seg[6]; static int g_segN = 0, g_drop = 0;

void GpuInit(ID3D10Device* dev) {
    if (g_dev) return;
    g_dev = dev;
    D3D10_QUERY_DESC qd{};
    for (auto& f : g_ring) {
        qd.Query = D3D10_QUERY_TIMESTAMP_DISJOINT; dev->CreateQuery(&qd, &f.disj);
        qd.Query = D3D10_QUERY_TIMESTAMP;
        for (auto& t : f.ts) dev->CreateQuery(&qd, &t);
    }
    LOG("perf: GPU timestamp queries created");
}

static bool TryRead(FrameQ& f) {
    D3D10_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (f.disj->GetData(&dj, sizeof dj, D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
    UINT64 t[G_COUNT]{};
    for (int i = 0; i < G_COUNT; ++i) {
        if (!f.issued[i]) continue;
        if (f.ts[i]->GetData(&t[i], sizeof(UINT64), D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
    }
    f.pending = false;
    bool all = true; for (int i = 0; i < G_COUNT; ++i) all = all && f.issued[i];
    if (dj.Disjoint || !all || !dj.Frequency) { ++g_drop; return true; }
    double k = 1000.0 / (double)dj.Frequency;
    g_seg[0] += (double)(t[G_HOOK] - t[G_FRAME]) * k;      // engine work before the 3D viewport (shadows? sim uploads)
    g_seg[1] += (double)(t[G_EYE0] - t[G_HOOK]) * k;       // left eye
    g_seg[2] += (double)(t[G_EYE1] - t[G_EYE0]) * k;       // right eye (+ left copy)
    g_seg[3] += (double)(t[G_PRESENT] - t[G_EYE1]) * k;    // copies, mirror, 2D/HUD
    g_seg[4] += (double)(t[G_PRESENT] - t[G_FRAME]) * k;   // whole frame (excl. Present itself)
    ++g_segN;
    return true;
}

void GpuMark(GpuPoint p) {
    if (!g_dev) return;
    if (p == G_FRAME) {
        if (g_cur >= 0 && g_ring[g_cur].open) { g_ring[g_cur].disj->End(); g_ring[g_cur].open = false; g_ring[g_cur].pending = true; }
        for (int i = 1; i <= kRing; ++i) { FrameQ& f = g_ring[(g_cur + i + kRing) % kRing]; if (f.pending) TryRead(f); }
        int n = (g_cur + 1) % kRing;
        FrameQ& f = g_ring[n];
        if (f.pending && !TryRead(f)) { ++g_drop; g_cur = -1; return; }   // still in flight: skip this frame
        g_cur = n;
        memset(f.issued, 0, sizeof f.issued);
        f.disj->Begin(); f.open = true;
    }
    if (g_cur < 0 || !g_ring[g_cur].open) return;
    g_ring[g_cur].ts[p]->End();
    g_ring[g_cur].issued[p] = true;
}

// ---------------------------------------------------------------- sampler
static volatile LONG g_phase = P_NONE;
void SetPhase(Phase p) { InterlockedExchange(&g_phase, p); }
static const char* kPhase[P_COUNT] = { "none", "sim", "rfPre", "xrWait", "eye0", "eye1", "hookMisc", "rfPost", "mid", "present" };

static uintptr_t g_dBase = 0, g_dTextEnd = 0;
static CRITICAL_SECTION g_cs;
struct Ent { uint64_t key; uint32_t a, b; uint32_t n; };
static const int kTab = 4096;
static Ent g_eip[kTab], g_call[kTab], g_incl[kTab];
// inclusive attribution: for each function of interest, which of its call sites is on the stack (Steam VA, size)
struct Focus { const char* name; uint32_t va, size; uintptr_t lo, hi; };
static Focus g_focus[] = {
    { "RenderScene A", 0x10356BC0, 0x1842 }, { "RenderScene B", 0x10347040, 0x1B8F }, { "MainDraw", 0x1038E6E0, 0x1E87 },
    { "SceneCull", 0x103766A0, 0x63B }, { "SunShadows", 0x103AE000, 0xBFA }, { "ShadowCollect", 0x103A9FE0, 0x305E }, { "WaterRefl", 0x103A8640, 0xACA }, { "RenderViewport", 0x1033F6F0, 0x161 } };
static const int kFocus = (int)(sizeof g_focus / sizeof g_focus[0]);
static uint32_t g_phaseN[P_COUNT];
static uint32_t g_total = 0;

static void Add(Ent* tab, uint64_t key, uint32_t a, uint32_t b) {
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 52) & (kTab - 1);
    for (int i = 0; i < kTab; ++i) {
        Ent& e = tab[(h + i) & (kTab - 1)];
        if (e.n == 0) { e.key = key; e.a = a; e.b = b; e.n = 1; return; }
        if (e.key == key) { ++e.n; return; }
    }
}
static bool IsDuniaRA(uintptr_t v) {
    if (v < g_dBase + 0x1000 || v >= g_dTextEnd) return false;
    const uint8_t* p = (const uint8_t*)v;
    return p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF;
}
static uint32_t ToVA(uintptr_t a) { return (uint32_t)(a - g_dBase + 0x10000000); }

static DWORD WINAPI SamplerThread(LPVOID p) {
    DWORD tid = (DWORD)(uintptr_t)p;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { LogWrite("perf: OpenThread failed %lu", GetLastError()); return 0; }
    timeBeginPeriod(1);
    static uintptr_t stack[4096];
    for (;;) {
        Sleep(1);
        LONG ph = g_phase;
        if (ph == P_NONE) continue;
        if (SuspendThread(th) == (DWORD)-1) continue;
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(th, &c);
        ph = g_phase;
        size_t words = 0;
        if (ok) {
            MEMORY_BASIC_INFORMATION m;
            if (VirtualQuery((void*)c.Esp, &m, sizeof m) && m.State == MEM_COMMIT) {
                size_t avail = ((uintptr_t)m.BaseAddress + m.RegionSize - c.Esp) / 4;
                words = avail < 4096 ? avail : 4096;
                memcpy(stack, (void*)c.Esp, words * 4);
            }
        }
        ResumeThread(th);
        if (!ok) continue;
        // first two Dunia return addresses on the stack
        uint32_t r1 = 0, r2 = 0;
        for (size_t i = 0; i < words && !r2; ++i) if (IsDuniaRA(stack[i])) { if (!r1) r1 = ToVA(stack[i]); else if (ToVA(stack[i]) != r1) r2 = ToVA(stack[i]); }
        uintptr_t eip = c.Eip;
        if (ph == P_EYE0 || ph == P_EYE1 || ph == P_HOOKMISC) {
            bool seen[16] = {};
            for (size_t i = 0; i < words; ++i) {
                uintptr_t v = stack[i];
                for (int f = 0; f < kFocus; ++f) if (!seen[f] && v > g_focus[f].lo && v < g_focus[f].hi && IsDuniaRA(v)) {
                    seen[f] = true;
                    EnterCriticalSection(&g_cs); Add(g_incl, ((uint64_t)f << 32) | (uint32_t)(v - g_focus[f].lo), f, (uint32_t)(v - g_focus[f].lo)); LeaveCriticalSection(&g_cs);
                }
            }
        }
        bool inD = eip >= g_dBase && eip < g_dTextEnd;
        EnterCriticalSection(&g_cs);
        ++g_total; ++g_phaseN[ph];
        if (inD) Add(g_eip, ((uint64_t)ph << 32) | (ToVA(eip) & ~0xFu), ToVA(eip) & ~0xFu, 0);
        else {
            HMODULE mod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)eip, &mod);
            Add(g_eip, ((uint64_t)ph << 32) | ((uintptr_t)mod | 1), (uint32_t)(uintptr_t)mod, 1);
            Add(g_call, ((uint64_t)ph << 56) ^ ((uint64_t)r1 << 24) ^ r2 ^ ((uint64_t)(uintptr_t)mod << 8), r1, r2 | 0);
        }
        LeaveCriticalSection(&g_cs);
    }
}

void StartSampler(DWORD mainTid) {
    static bool started = false;
    if (started) return;
    started = true;
    HMODULE d = GetModuleHandleA("Dunia.dll");
    g_dBase = (uintptr_t)d; g_dTextEnd = g_dBase + 0xE00000;
    for (int f = 0; f < kFocus; ++f) { uint32_t v = gameaddr::Xlate(g_focus[f].va); g_focus[f].lo = v ? g_dBase + (v - 0x10000000) : 0; g_focus[f].hi = v ? g_focus[f].lo + g_focus[f].size : 0; }
    InitializeCriticalSection(&g_cs);
    HANDLE h = CreateThread(nullptr, 0, SamplerThread, (LPVOID)(uintptr_t)mainTid, 0, nullptr);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(h); }
    LOG("perf: sampler started on thread %lu", mainTid);
}

static void ModName(uint32_t mod, char* out, size_t n) {
    char p[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA((HMODULE)(uintptr_t)mod, p, MAX_PATH);
    const char* b = strrchr(p, '\\'); snprintf(out, n, "%s", b ? b + 1 : p);
}

void Report() {
    if (g_segN) {
        double k = 1.0 / g_segN;
        LOG("GPU avg %d frames (ms): preViewport=%.2f eye0=%.2f eye1=%.2f post=%.2f total=%.2f (dropped %d)",
            g_segN, g_seg[0] * k, g_seg[1] * k, g_seg[2] * k, g_seg[3] * k, g_seg[4] * k, g_drop);
        memset(g_seg, 0, sizeof g_seg); g_segN = 0; g_drop = 0;
    }
    if (!g_dBase) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 10000) return;
    last = now;
    static Ent a[kTab], b[kTab], ic[kTab]; static uint32_t pn[P_COUNT]; uint32_t tot;
    EnterCriticalSection(&g_cs);
    memcpy(a, g_eip, sizeof a); memcpy(b, g_call, sizeof b); memcpy(ic, g_incl, sizeof ic); memcpy(pn, g_phaseN, sizeof pn); tot = g_total;
    memset(g_eip, 0, sizeof g_eip); memset(g_call, 0, sizeof g_call); memset(g_incl, 0, sizeof g_incl); memset(g_phaseN, 0, sizeof g_phaseN); g_total = 0;
    LeaveCriticalSection(&g_cs);
    if (!tot) return;
    char line[512]; int o = snprintf(line, sizeof line, "SAMPLES %u:", tot);
    for (int i = 1; i < P_COUNT; ++i) o += snprintf(line + o, sizeof line - o, " %s=%.1f%%", kPhase[i], 100.0 * pn[i] / tot);
    LogWrite("%s", line);
    auto top = [&](Ent* t, const char* title, bool call) {
        std::sort(t, t + kTab, [](const Ent& x, const Ent& y) { return x.n > y.n; });
        LogWrite(" top %s:", title);
        for (int i = 0; i < 30 && t[i].n; ++i) {
            int ph = call ? (int)(t[i].key >> 56) : (int)(t[i].key >> 32);
            if (ph < 0 || ph >= P_COUNT) ph = 0;
            if (!call) {
                if (t[i].b) { char m[64]; ModName(t[i].a, m, 64); LogWrite("  %5.1f%% [%s] %s", 100.0 * t[i].n / tot, kPhase[ph], m); }
                else LogWrite("  %5.1f%% [%s] Dunia %08x", 100.0 * t[i].n / tot, kPhase[ph], t[i].a);
            } else {
                char m[64]; ModName((uint32_t)((t[i].key >> 8) & 0xFFFFFFFF) , m, 64);
                LogWrite("  %5.1f%% [%s] via %08x <- %08x", 100.0 * t[i].n / tot, kPhase[ph], t[i].a, t[i].b);
            }
        }
    };
    {   // inclusive time per call site inside the functions of interest
        std::sort(ic, ic + kTab, [](const Ent& x, const Ent& y) { return x.a != y.a ? x.a < y.a : x.n > y.n; });
        for (int f = 0; f < kFocus; ++f) {
            uint32_t sum = 0; for (int i = 0; i < kTab; ++i) if (ic[i].n && (int)ic[i].a == f) sum += ic[i].n;
            if (!sum) continue;
            LogWrite(" inclusive %s (%08x): %.1f%% of all samples", g_focus[f].name, g_focus[f].va, 100.0 * sum / tot);
            int shown = 0;
            for (int i = 0; i < kTab && shown < 14; ++i) if (ic[i].n && (int)ic[i].a == f && ic[i].n * 200 >= sum) { ++shown; LogWrite("    %5.1f%% call site +%04x", 100.0 * ic[i].n / tot, ic[i].b); }
        }
    }
    top(a, "EIP", false);
    top(b, "callers of non-Dunia EIP (first two Dunia return addrs)", true);
}

} // namespace perf
