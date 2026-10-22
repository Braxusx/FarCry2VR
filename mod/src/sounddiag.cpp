// sounddiag.cpp - v203 diagnostic: which sounds the game plays around a machete attack.
// The sound manager (singleton at 0x11648F04, vtable 0x10E82D10, 56 slots; slot 0xA0 is what the script
// function PlaySoundAtPosition calls with a 32-bit sound id) gets a logging thunk on every vtable slot.
// For 1.5 s after the right trigger is pulled with the machete out, each call is recorded (slot, caller,
// first four stack arguments) and summarised in the log when the window closes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "log.h"
#include "gameaddr.h"
#include "controls.h"

namespace sounddiag {
namespace {
uintptr_t A(uintptr_t va) {
    static uintptr_t base = 0; if (!base) base = (uintptr_t)GetModuleHandleA("Dunia.dll");
    uint32_t v = gameaddr::Xlate((uint32_t)va); return (v && base) ? base + (v - 0x10000000) : 0;
}
const int kSlots = 56;
void* g_orig[kSlots];
uint8_t* g_stubs = nullptr;
volatile LONG g_window = 0;   // 1 while recording
struct Rec { int slot; uint32_t ret, a[4]; DWORD tid; };
Rec g_rec[4096]; volatile LONG g_n = 0;
bool g_installed = false;

// (slot, caller) pairs seen in normal play, outside the attack windows: the routine per-frame traffic
volatile LONG g_seen[16384];
DWORD g_learnUntil = 0;
bool SeenOrAdd(int slot, uint32_t ret, bool add) {
    LONG key = (LONG)((ret ^ ((uint32_t)slot << 24)) | 1u);
    uint32_t h = ((ret >> 2) * 2654435761u ^ (uint32_t)slot * 40503u) & 16383;
    for (int i = 0; i < 64; ++i) {
        LONG v = g_seen[(h + i) & 16383];
        if (v == key) return true;
        if (v == 0) { if (add) InterlockedCompareExchange(&g_seen[(h + i) & 16383], key, 0); return false; }
    }
    return false;
}
extern "C" void __cdecl SoundDiagLog(int slot, uint32_t* s) {
    if (!g_window) { if (!controls::MacheteSelected()) SeenOrAdd(slot, s[0], true); return; }   // learn only without the machete out (its swing must stay new)
    if (SeenOrAdd(slot, s[0], false)) return;   // routine traffic
    LONG i = InterlockedIncrement(&g_n) - 1;
    if (i >= 4096) return;
    Rec& r = g_rec[i]; r.slot = slot; r.ret = s[0];
    for (int k = 0; k < 4; ++k) r.a[k] = IsBadReadPtr(s + 1 + k, 4) ? 0 : s[1 + k];
    r.tid = GetCurrentThreadId();
}

bool Install() {
    uintptr_t slot = A(0x11648F04); if (!slot) return false;
    uint8_t* mgr = *(uint8_t**)slot;
    if (!mgr || IsBadReadPtr(mgr, 4)) return false;
    void** vt = *(void***)mgr;
    if (IsBadReadPtr(vt, kSlots * 4)) return false;
    g_stubs = (uint8_t*)VirtualAlloc(nullptr, kSlots * 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) return false;
    DWORD old; if (!VirtualProtect(vt, kSlots * 4, PAGE_READWRITE, &old)) return false;
    for (int i = 0; i < kSlots; ++i) {
        g_orig[i] = vt[i];
        uint8_t* q = g_stubs + i * 48;
        *q++ = 0x60;                                    // pushad
        *q++ = 0x9C;                                    // pushfd
        *q++ = 0x8D; *q++ = 0x44; *q++ = 0x24; *q++ = 0x24;   // lea eax,[esp+24h]  (the return address)
        *q++ = 0x50;                                    // push eax
        *q++ = 0x68; int32_t sl = i; memcpy(q, &sl, 4); q += 4;   // push slot
        *q++ = 0xE8; int32_t rel = (int32_t)((uint8_t*)&SoundDiagLog - (q + 4)); memcpy(q, &rel, 4); q += 4;   // call
        *q++ = 0x83; *q++ = 0xC4; *q++ = 0x08;          // add esp,8
        *q++ = 0x9D;                                    // popfd
        *q++ = 0x61;                                    // popad
        *q++ = 0xFF; *q++ = 0x25; void** p = &g_orig[i]; memcpy(q, &p, 4); q += 4;   // jmp [g_orig[i]]
        vt[i] = g_stubs + i * 48;
    }
    VirtualProtect(vt, kSlots * 4, old, &old);
    LOG("sound diag: manager %p, vtable %p - %d slots logged during machete attacks (1.5 s after the right trigger)", mgr, vt, kSlots);
    return true;
}

void Report() {
    LONG n = g_n; if (n > 4096) n = 4096;
    LOG("sound diag: ===== machete attack window: %ld calls not seen in normal play =====", (long)n);
    // one line per distinct (slot, caller, arg1), in the order first seen
    bool done[4096] = {};
    for (LONG i = 0; i < n; ++i) {
        if (done[i]) continue;
        int cnt = 0; DWORD t0 = 0;
        for (LONG j = i; j < n; ++j) if (!done[j] && g_rec[j].slot == g_rec[i].slot && g_rec[j].ret == g_rec[i].ret && g_rec[j].a[0] == g_rec[i].a[0]) { done[j] = true; ++cnt; }
        (void)t0;
        const Rec& r = g_rec[i];
        LOG("sound diag:   slot 0x%02X x%d  caller %08X  args %08X %08X %08X %08X  tid %lu", r.slot * 4, cnt, r.ret, r.a[0], r.a[1], r.a[2], r.a[3], r.tid);
    }
}
} // namespace

// main thread, every frame
void Tick() {
    static DWORD nextTry = 0;
    if (!g_installed) {
        if (GetTickCount() < nextTry) return;
        nextTry = GetTickCount() + 2000;
        g_installed = Install();
        if (!g_installed) return;
    }
    static bool prev = false; static DWORD until = 0;
    bool pulled = controls::RightTriggerValue() > 0.6f;
    if (!g_learnUntil) g_learnUntil = GetTickCount() + 15000;
    if (pulled && !prev && controls::MacheteSelected() && !g_window && GetTickCount() < g_learnUntil) LOG("sound diag: still learning the normal sound traffic - attack again in a few seconds");
    if (pulled && !prev && controls::MacheteSelected() && !g_window && GetTickCount() >= g_learnUntil) {
        g_n = 0; until = GetTickCount() + 1500; InterlockedExchange(&g_window, 1);
        LOG("sound diag: right trigger with the machete - recording");
    }
    prev = pulled;
    if (g_window && GetTickCount() >= until) { InterlockedExchange(&g_window, 0); Sleep(0); Report(); }
}
}
