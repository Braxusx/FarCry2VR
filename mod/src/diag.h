#pragma once
// v230: frame pacing / system diagnostics (PACE lines in fc2vr.log) and the Windows 11 timer-resolution fix.
#include <stdint.h>
struct ID3D10Device;
namespace diag {
void Startup(const char* iniPath);                 // process, timer and power-throttling state (+ the fix, ini TimerFix)
void OnWaitFrame(double waitMs, int64_t predictedPeriodNs, bool shouldRender);
void OnEndFrame(double endMs);
void GpuFrameStart(ID3D10Device* dev);           // after xrBeginFrame
void GpuFrameEnd();                               // before xrEndFrame
}
