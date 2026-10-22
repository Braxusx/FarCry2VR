// FC2VR v230 - frame pacing and system diagnostics.
//
// Every ~10 s in VR a "PACE" line reports: the real frame rate, the refresh period the OpenXR runtime paces us to,
// how long xrWaitFrame / xrEndFrame block, the GPU time of the frame (D3D10 timestamps), the current Windows timer
// resolution, which app has the foreground and whether the game window is minimized.
//
// Windows 11 ignores a process's timer-resolution request (timeBeginPeriod) while its window is minimized or hidden:
// sleeps then round up to 15.6 ms, which caps a frame loop at 64 fps. In VR the game window is often in the
// background, so at start-up (ini TimerFix=1, the default) the process opts out of that throttling
// (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION and EXECUTION_SPEED) and asks for a 1 ms timer.
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include <string.h>
#include "diag.h"
#include "log.h"

namespace diag {

static double g_qpcMs;
static double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * g_qpcMs; }

typedef LONG(NTAPI* NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
static NtQueryTimerResolution_t pNtQTR;
static double TimerMs() {
    ULONG mn = 0, mx = 0, cur = 0;
    if (pNtQTR && pNtQTR(&mn, &mx, &cur) == 0) return cur / 10000.0;
    return -1;
}

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
typedef BOOL(WINAPI* ProcInfo_t)(HANDLE, int, LPVOID, DWORD);
static const int kProcessPowerThrottling = 4;   // PROCESS_INFORMATION_CLASS

static const char* PrioName(DWORD p) {
    switch (p) {
    case IDLE_PRIORITY_CLASS: return "idle"; case BELOW_NORMAL_PRIORITY_CLASS: return "below normal"; case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above normal"; case HIGH_PRIORITY_CLASS: return "high"; case REALTIME_PRIORITY_CLASS: return "realtime";
    } return "?";
}

void Startup(const char* ini) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart;
    pNtQTR = (NtQueryTimerResolution_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    HKEY k; char cpu[128] = "?"; DWORD sz = sizeof(cpu);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, "ProcessorNameString", nullptr, nullptr, (BYTE*)cpu, &sz); RegCloseKey(k);
    }
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*); OSVERSIONINFOW v{ sizeof v };
    auto rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"); if (rgv) rgv(&v);
    SYSTEM_POWER_STATUS ps{}; GetSystemPowerStatus(&ps);
    LOG("system: Windows %lu.%lu build %lu, CPU %s, power %s, process priority %s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, cpu,
        ps.ACLineStatus == 1 ? "AC" : ps.ACLineStatus == 0 ? "BATTERY" : "unknown", PrioName(GetPriorityClass(GetCurrentProcess())));
    auto getPI = (ProcInfo_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessInformation");
    auto setPI = (ProcInfo_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    PROCESS_POWER_THROTTLING_STATE st{ PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0 };
    if (getPI && getPI(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof st))
        LOG("timer: resolution %.3f ms at start-up, power throttling control 0x%lx state 0x%lx", TimerMs(), st.ControlMask, st.StateMask);
    else LOG("timer: resolution %.3f ms at start-up", TimerMs());
    int fix = ini ? (int)GetPrivateProfileIntA("VR", "TimerFix", 1, ini) : 1;
    if (fix) {
        PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                          PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
        BOOL ok = setPI ? setPI(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof s) : FALSE;
        if (!ok) {   // older Windows 10 builds don't know the timer flag
            s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            ok = setPI ? setPI(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof s) : FALSE;
        }
        MMRESULT tr = timeBeginPeriod(1);
        LOG("timer fix (TimerFix=1): power throttling off %s (mask 0x%lx), timeBeginPeriod(1) %s, resolution now %.3f ms", ok ? "ok" : "failed",
            s.ControlMask, tr == TIMERR_NOERROR ? "ok" : "failed", TimerMs());
    } else LOG("timer fix off (TimerFix=0)");
}

// ---- pacing statistics ----------------------------------------------------------------------------------------
static double g_t0, g_lastWait, g_wait, g_end, g_endMax, g_minFt = 1e9, g_maxFt, g_period;
static long g_frames, g_noRender, g_endN;
static double g_gpu; static long g_gpuN;

static void ForegroundName(char* out, size_t n) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!fg) { snprintf(out, n, "none"); return; }
    if (pid == GetCurrentProcessId()) { snprintf(out, n, "the game"); return; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    char path[MAX_PATH] = ""; DWORD len = MAX_PATH;
    if (h) { QueryFullProcessImageNameA(h, 0, path, &len); CloseHandle(h); }
    const char* b = strrchr(path, '\\');
    snprintf(out, n, "%s", path[0] ? (b ? b + 1 : path) : "another app");
}
struct WinFind { DWORD pid; HWND best; long area; };
static BOOL CALLBACK EnumWin(HWND w, LPARAM lp) {
    WinFind* f = (WinFind*)lp; DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    RECT r; GetWindowRect(w, &r); long a = (r.right - r.left) * (r.bottom - r.top);
    if (IsIconic(w) || a > f->area) { if (a > f->area || !f->best) { f->best = w; f->area = a; } }
    return TRUE;
}

static void Report(double now) {
    double secs = (now - g_t0) / 1000.0;
    if (secs <= 0 || g_frames < 2) return;
    char fg[96]; ForegroundName(fg, sizeof fg);
    WinFind wf{ GetCurrentProcessId(), nullptr, 0 }; EnumWindows(EnumWin, (LPARAM)&wf);
    const char* win = !wf.best ? "not found" : IsIconic(wf.best) ? "MINIMIZED" : "shown";
    double hz = g_period > 0 ? 1000.0 / g_period : 0;
    LOG("PACE %.1f s: %.1f fps (frame %.2f-%.2f ms), runtime refresh %.2f Hz, frames not shown %ld, xrWaitFrame %.2f ms, xrEndFrame %.2f ms (max %.2f), "
        "GPU %.2f ms, timer %.3f ms, foreground: %s, game window %s, priority %s",
        secs, (g_frames - 1) / secs, g_minFt, g_maxFt, hz, g_noRender, g_wait / g_frames, g_endN ? g_end / g_endN : 0.0, g_endMax,
        g_gpuN ? g_gpu / g_gpuN : -1.0, TimerMs(), fg, win, PrioName(GetPriorityClass(GetCurrentProcess())));
    g_t0 = now; g_frames = 0; g_noRender = 0; g_wait = g_end = g_endMax = 0; g_endN = 0; g_minFt = 1e9; g_maxFt = 0; g_gpu = 0; g_gpuN = 0;
}

void OnWaitFrame(double waitMs, int64_t periodNs, bool shouldRender) {
    double now = Now();
    if (!g_t0) g_t0 = now;
    if (g_lastWait > 0) { double ft = now - g_lastWait; if (ft < g_minFt) g_minFt = ft; if (ft > g_maxFt) g_maxFt = ft; }
    g_lastWait = now;
    g_frames++; g_wait += waitMs; if (!shouldRender) g_noRender++;
    if (periodNs > 0) g_period = periodNs / 1e6;
    if (now - g_t0 >= 10000.0) Report(now);
}
void OnEndFrame(double endMs) { g_end += endMs; g_endN++; if (endMs > g_endMax) g_endMax = endMs; }

// ---- GPU time of the frame: D3D10 timestamps from xrBeginFrame to xrEndFrame, read back a few frames later ----
enum { RING = 8 };
struct GpuQ { ID3D10Query *dis, *t0, *t1; bool pending; };
static GpuQ g_q[RING];
static ID3D10Device* g_dev;
static int g_cur = -1, g_next;
static bool g_gpuOff;

static void Collect() {
    for (int i = 0; i < RING; i++) {
        GpuQ& q = g_q[i];
        if (!q.pending) continue;
        D3D10_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
        if (q.dis->GetData(&dj, sizeof dj, D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (q.t0->GetData(&a, sizeof a, D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK || q.t1->GetData(&b, sizeof b, D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_gpu += (b - a) * 1000.0 / dj.Frequency; g_gpuN++; }
    }
}
void GpuFrameStart(ID3D10Device* dev) {
    if (g_gpuOff || !dev) return;
    if (dev != g_dev) {
        for (auto& q : g_q) { if (q.dis) q.dis->Release(); if (q.t0) q.t0->Release(); if (q.t1) q.t1->Release(); q = GpuQ{}; }
        g_dev = dev;
        D3D10_QUERY_DESC dd{ D3D10_QUERY_TIMESTAMP_DISJOINT, 0 }, td{ D3D10_QUERY_TIMESTAMP, 0 };
        for (auto& q : g_q)
            if (FAILED(dev->CreateQuery(&dd, &q.dis)) || FAILED(dev->CreateQuery(&td, &q.t0)) || FAILED(dev->CreateQuery(&td, &q.t1))) { g_gpuOff = true; LOG("PACE: GPU timestamps unavailable"); return; }
    }
    Collect();
    GpuQ& q = g_q[g_next];
    if (q.pending) { g_cur = -1; return; }   // results still outstanding: skip this frame
    g_cur = g_next; g_next = (g_next + 1) % RING;
    q.dis->Begin(); q.t0->End();
}
void GpuFrameEnd() {
    if (g_cur < 0 || g_gpuOff) return;
    GpuQ& q = g_q[g_cur];
    q.t1->End(); q.dis->End(); q.pending = true; g_cur = -1;
}

}  // namespace diag
