#include "hang.h"
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
void LogWrite(const char* fmt, ...);
namespace hang {
static char g_dir[MAX_PATH];
static volatile LONG64 g_lastBeat = 0;     // GetTickCount64 of the last frame
static volatile DWORD g_mainTid = 0;
static const char* volatile g_mark = "start";
static volatile LONG g_frames = 0;
void Beat() { g_lastBeat = (LONG64)GetTickCount64(); g_mainTid = GetCurrentThreadId(); InterlockedIncrement(&g_frames); }
void Mark(const char* where) { g_mark = where; }

static void ModOff(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char p[MAX_PATH] = "?"; GetModuleFileNameA(m, p, MAX_PATH); const char* b = strrchr(p, '\\'); b = b ? b + 1 : p;
        if (!_stricmp(b, "Dunia.dll")) snprintf(out, n, "Dunia!%08x", (unsigned)(a - (uintptr_t)m + 0x10000000));
        else snprintf(out, n, "%s+%x", b, (unsigned)(a - (uintptr_t)m));
    } else snprintf(out, n, "%08x", (unsigned)a);
}
static bool IsExec(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)a, &mb, sizeof mb) || mb.State != MEM_COMMIT) return false;
    return (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 && mb.Type == MEM_IMAGE;
}
static bool LooksLikeRet(uintptr_t a) {
    if (a < 0x10000 || !IsExec(a - 7)) return false;
    const uint8_t* p = (const uint8_t*)a;
    return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}
static void DumpThreads(FILE* f) {
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof te };
    static uintptr_t stack[16384];
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        FILETIME c, e, k, u; ULONGLONG cpu = 0;
        if (GetThreadTimes(th, &c, &e, &k, &u)) cpu = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
        if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        BOOL got = GetThreadContext(th, &ctx);
        size_t words = 0;
        if (got) {
            MEMORY_BASIC_INFORMATION mb;
            if (VirtualQuery((void*)ctx.Esp, &mb, sizeof mb) && mb.State == MEM_COMMIT) {
                size_t avail = ((uintptr_t)mb.BaseAddress + mb.RegionSize - ctx.Esp) / 4;
                words = avail < 16384 ? avail : 16384;
                memcpy(stack, (void*)ctx.Esp, words * 4);
            }
        }
        ResumeThread(th);
        char eip[96]; ModOff(got ? ctx.Eip : 0, eip, sizeof eip);
        fprintf(f, "\nThread %lu%s  prio %ld  cpu %llu ms  EIP %s  ESP %08lx EBP %08lx  EAX %08lx ECX %08lx EDX %08lx\n",
            te.th32ThreadID, te.th32ThreadID == g_mainTid ? "  [RENDER/MAIN]" : "", te.tpBasePri, cpu, eip,
            got ? ctx.Esp : 0, got ? ctx.Ebp : 0, got ? ctx.Eax : 0, got ? ctx.Ecx : 0, got ? ctx.Edx : 0);
        int shown = 0;
        for (size_t i = 0; i < words && shown < 40; ++i) {
            uintptr_t v = stack[i];
            if (!LooksLikeRet(v)) continue;
            char m[96]; ModOff(v, m, sizeof m);
            fprintf(f, "   [esp+%04x] %s\n", (unsigned)(i * 4), m); ++shown;
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}
void Report(const char* why, bool dump, EXCEPTION_POINTERS* ep) {
    static int n = 0; ++n;
    char path[MAX_PATH]; snprintf(path, MAX_PATH, "%sfc2vr_hang_%d.txt", g_dir, n);
    FILE* f = fopen(path, "w");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "FC2VR hang report %d (%s) at %02d:%02d:%02d\nlast frame %lld ms ago, frames so far %ld, last mod checkpoint on the render thread: %s\n",
            n, why, st.wHour, st.wMinute, st.wSecond, (long long)GetTickCount64() - g_lastBeat, g_frames, g_mark);
        if (ep) {
            char w[96]; ModOff((uintptr_t)ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
            fprintf(f, "\nCRASH: exception %08lx at %s on thread %lu (info %08lx %08lx)\n", ep->ExceptionRecord->ExceptionCode, w, GetCurrentThreadId(),
                ep->ExceptionRecord->NumberParameters > 0 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[0] : 0ul,
                ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul);
            const uintptr_t* sp = (const uintptr_t*)ep->ContextRecord->Esp; int shown = 0;
            MEMORY_BASIC_INFORMATION mb;
            if (VirtualQuery(sp, &mb, sizeof mb) && mb.State == MEM_COMMIT) {
                size_t words = ((uintptr_t)mb.BaseAddress + mb.RegionSize - (uintptr_t)sp) / 4; if (words > 16384) words = 16384;
                for (size_t i = 0; i < words && shown < 40; ++i) if (LooksLikeRet(sp[i])) { char m[96]; ModOff(sp[i], m, sizeof m); fprintf(f, "   [esp+%04x] %s\n", (unsigned)(i * 4), m); ++shown; }
            }
        }
        DumpThreads(f);
        fclose(f);
    }
    LogWrite("HANG REPORT %d (%s): %s, checkpoint '%s'", n, why, path, g_mark);
    if (dump) {
        char dp[MAX_PATH]; snprintf(dp, MAX_PATH, "%sfc2vr_hang_%d.dmp", g_dir, n);
        HMODULE dbg = LoadLibraryA("dbghelp.dll");
        typedef BOOL (WINAPI *MDW_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        MDW_t mdw = dbg ? (MDW_t)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
        HANDLE h = CreateFileA(dp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mdw && h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
            BOOL ok = mdw(GetCurrentProcess(), GetCurrentProcessId(), h,
                (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules), ep ? &mei : nullptr, nullptr, nullptr);
            LogWrite("minidump %s: %d", dp, ok);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
}
static bool g_watchFrames = true;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) Report("crash", true, ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI Watch(LPVOID) {
    int stage = 0; bool prevKey = false;
    for (;;) {
        Sleep(250);
        bool key = (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        if (key && !prevKey) Report("Ctrl+Shift+F12", true);
        prevKey = key;
        if (!g_lastBeat || !g_watchFrames) continue;
        LONG64 idle = (LONG64)GetTickCount64() - g_lastBeat;
        if (idle < 1000) { if (stage) LogWrite("frames resumed (stall ended)"); stage = 0; continue; }
        if (stage == 0 && idle > 5000) { stage = 1; Report("no frame for 5 s", false); }
        else if (stage == 1 && idle > 15000) { stage = 2; Report("no frame for 15 s", true); }
        else if (stage == 2 && idle > 45000) { stage = 3; Report("no frame for 45 s", false); }
    }
}
void Start(const char* dir, bool watchFrames) {
    static bool started = false; if (started) return; started = true;
    snprintf(g_dir, MAX_PATH, "%s", dir);
    g_watchFrames = watchFrames;
    SetUnhandledExceptionFilter(CrashFilter);
    HANDLE h = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
} // namespace hang
