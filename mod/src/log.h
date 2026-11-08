#pragma once
void LogWrite(const char* fmt, ...);
#define LOG(...) LogWrite(__VA_ARGS__)
// 1.0.4: periodic diagnostics only with DiagLog=1 in fc2vr.ini (they were test tooling)
extern volatile bool g_diagLog;
#define DLOG(...) do { if (g_diagLog) LogWrite(__VA_ARGS__); } while (0)
