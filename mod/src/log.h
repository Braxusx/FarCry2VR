#pragma once
void LogWrite(const char* fmt, ...);
#define LOG(...) LogWrite(__VA_ARGS__)
