#include "gameaddr.h"
#include "log.h"
#include <windows.h>

namespace gameaddr {
#include "gogmap.inc"
static volatile LONG g_build = -1;

int Detect() {
    if (g_build >= 0) return g_build;
    HMODULE m = GetModuleHandleA("Dunia.dll");
    if (!m) return Unknown;                       // not loaded yet: decide later
    const uint8_t* b = (const uint8_t*)m;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp, sz = nt->OptionalHeader.SizeOfImage;
    int v = ts == 0x4AAE9636 ? Steam : ts == 0x49FB4BF6 ? GOG : Unknown;
    if (v == Unknown) v = sz == 0x019E8000 ? Steam : sz == 0x0192A000 ? GOG : Unknown;   // fallback: image size
    InterlockedExchange(&g_build, v);
    LOG("Dunia.dll build: %s (timestamp %08x, image size %08x)%s", Name(), (unsigned)ts, (unsigned)sz,
        v == Unknown ? " - unknown version, using Steam addresses; patches check their bytes and skip on mismatch" : "");
    return v;
}
const char* Name() { return g_build == Steam ? "Steam" : g_build == GOG ? "GOG" : "unknown"; }

uint32_t Xlate(uint32_t va) {
    int b = g_build >= 0 ? (int)g_build : Detect();
    if (b != GOG) return va;
    int lo = 0, hi = (int)(sizeof kGogMap / sizeof kGogMap[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kGogMap[mid][0] == va) return kGogMap[mid][1];
        if (kGogMap[mid][0] < va) lo = mid + 1; else hi = mid - 1;
    }
    static uint32_t logged[32]; static int nl = 0;
    bool seen = false; for (int i = 0; i < nl; ++i) seen |= logged[i] == va;
    if (!seen && nl < 32) { logged[nl++] = va; LOG("GOG: no address for Steam %08x", (unsigned)va); }
    return 0;
}
}
