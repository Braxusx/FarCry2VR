// hang.h - freeze watchdog: when the game stops presenting frames, write a report of every thread's stack
// (fc2vr_hang_N.txt) and a minidump (fc2vr_hang_N.dmp) next to the DLL. Ctrl+Shift+F12 writes one on demand.
#pragma once
#include <windows.h>
namespace hang {
void Start(const char* dir, bool watchFrames);
void Beat();                    // call once per presented frame (render thread)
void Mark(const char* where);   // last mod checkpoint passed on the render thread
// Writes fc2vr_hang_N.txt (all thread stacks) and, if dump, fc2vr_hang_N.dmp. ep: optional crash context.
void Report(const char* why, bool dump, EXCEPTION_POINTERS* ep = nullptr);
}
