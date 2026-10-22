// Symbols the MSVC-built NGX static library (nvsdk_ngx_s.lib) expects from the MSVC CRT,
// supplied for a mingw (llvm-mingw, UCRT) link.
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
extern "C" {
int _fltused = 0x9875;
uintptr_t __security_cookie = 0x00002B992DDFA232ull;
void __security_check_cookie(uintptr_t c) { if (c != __security_cookie) __fastfail(2); }
EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD, void*, PCONTEXT, void*) { return ExceptionContinueSearch; }
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(PEXCEPTION_RECORD, void*, PCONTEXT, void*) { return ExceptionContinueSearch; }
// thread-safe local statics (MSVC "magic statics")
thread_local int _Init_thread_epoch = INT_MIN;
static int g_epoch = INT_MIN;
static SRWLOCK g_lock = SRWLOCK_INIT;
void _Init_thread_header(int* p) {
    AcquireSRWLockExclusive(&g_lock);
    if (*p == 0) *p = -1;
    else { while (*p == -1) { ReleaseSRWLockExclusive(&g_lock); Sleep(0); AcquireSRWLockExclusive(&g_lock); } _Init_thread_epoch = g_epoch; }
    ReleaseSRWLockExclusive(&g_lock);
}
void _Init_thread_footer(int* p) {
    AcquireSRWLockExclusive(&g_lock);
    ++g_epoch; *p = g_epoch; _Init_thread_epoch = g_epoch;
    ReleaseSRWLockExclusive(&g_lock);
}
void _Init_thread_abort(int* p) { AcquireSRWLockExclusive(&g_lock); *p = 0; ReleaseSRWLockExclusive(&g_lock); }
void* msvc_new(size_t n) __asm__("??2@YAPEAX_K@Z");
void* msvc_new(size_t n) { void* p = malloc(n ? n : 1); if (!p) __fastfail(7); return p; }
void msvc_delete(void* p) __asm__("??3@YAXPEAX@Z");
void msvc_delete(void* p) { free(p); }
void msvc_delete_sz(void* p, size_t) __asm__("??3@YAXPEAX_K@Z");
void msvc_delete_sz(void* p, size_t) { free(p); }
void msvc_xbad_alloc() __asm__("?_Xbad_alloc@std@@YAXXZ");
void msvc_xbad_alloc() { __fastfail(7); }
void msvc_xlength(const char*) __asm__("?_Xlength_error@std@@YAXPEBD@Z");
void msvc_xlength(const char*) { __fastfail(7); }
}
