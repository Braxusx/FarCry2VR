// perf.h - GPU timestamp segments and a main-thread sampling profiler (diagnostics only).
#pragma once
#include <windows.h>
#include <d3d10_1.h>

namespace perf {

enum GpuPoint { G_FRAME = 0, G_HOOK, G_EYE0, G_EYE1, G_PRESENT, G_COUNT };
enum Phase { P_NONE = 0, P_SIM, P_RFPRE, P_WAIT, P_EYE0, P_EYE1, P_HOOKMISC, P_RFPOST, P_MID, P_PRESENT, P_COUNT };

void GpuInit(ID3D10Device* dev);
void GpuMark(GpuPoint p);        // G_FRAME closes the previous frame and opens a new one
void SetPhase(Phase p);
void StartSampler(DWORD mainTid);
void Report();                   // call about once per second from the render thread

} // namespace perf
