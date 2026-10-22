// Far Cry 2 VR launcher - extended system report for the log zip (system-info.txt).
// Covers what can cap or slow the VR frame rate outside the mod: displays and refresh rates, GPU driver, NVIDIA
// driver profile settings (frame-rate limiter, V-Sync, low latency), Windows graphics settings (HAGS, Game Mode,
// Game DVR, per-app GPU choice, compatibility flags), power plan, OpenXR runtimes and running overlay / limiter /
// VR tools. Only the listed tool names are reported, not the whole process list.
#include "game.h"
#include <windows.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <algorithm>

namespace fc2 {

static std::string Fmt(const char* f, ...) {
    char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}
static std::wstring LowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static bool RegDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& out, REGSAM view = 0) {
    HKEY k; if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return false;
    DWORD t = 0, sz = sizeof out; LONG r = RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&out, &sz); RegCloseKey(k);
    return r == ERROR_SUCCESS && t == REG_DWORD;
}
static std::string RegDwordText(HKEY root, const wchar_t* path, const wchar_t* name, REGSAM view = 0) {
    DWORD v; return RegDword(root, path, name, v, view) ? std::to_string(v) : std::string("not set");
}
// all values of a key whose name or data contains one of the needles (lower case)
static void RegValuesMatching(std::string& out, HKEY root, const wchar_t* path, const char* label, std::initializer_list<const wchar_t*> needles, REGSAM view = 0) {
    HKEY k; if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    wchar_t name[1024]; BYTE data[4096];
    for (DWORD i = 0;; ++i) {
        DWORD nl = 1024, dl = sizeof data - 2, t = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &t, data, &dl) != ERROR_SUCCESS) break;
        std::wstring d = (t == REG_SZ || t == REG_EXPAND_SZ) ? std::wstring((wchar_t*)data, dl / 2) : L"";
        while (!d.empty() && d.back() == 0) d.pop_back();
        std::wstring hay = LowerW(std::wstring(name) + L" " + d);
        bool hit = needles.size() == 0;
        for (auto n : needles) if (hay.find(n) != std::wstring::npos) hit = true;
        if (!hit) continue;
        out += Fmt("%s: %s", label, Narrow(name).c_str());
        if (t == REG_DWORD) out += Fmt(" = %lu", *(DWORD*)data); else if (!d.empty()) out += " = " + Narrow(d);
        out += "\r\n";
    }
    RegCloseKey(k);
}
static std::string FileVersion(const std::wstring& path) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return "";
    std::vector<BYTE> b(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data())) return "";
    VS_FIXEDFILEINFO* fi = nullptr; UINT l = 0;
    if (!VerQueryValueW(b.data(), L"\\", (void**)&fi, &l) || !fi) return "";
    return Fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

// ---- NVIDIA driver profile settings (NVAPI DRS, read only) -----------------------------------------------------
typedef void* (__cdecl* NvQI_t)(unsigned);
typedef int(__cdecl* NvInit_t)();
typedef int(__cdecl* NvSess_t)(void**);
typedef int(__cdecl* NvLoad_t)(void*);
typedef int(__cdecl* NvGlobal_t)(void*, void**);
typedef int(__cdecl* NvFindApp_t)(void*, const wchar_t*, void**, void*);
typedef int(__cdecl* NvEnum_t)(void*, void*, unsigned, unsigned*, void*);
typedef int(__cdecl* NvProfInfo_t)(void*, void*, void*);
typedef int(__cdecl* NvDrv_t)(unsigned*, char*);
enum { SETTING_SIZE = 0x3020, PROFILE_SIZE = 4116 };

static const char* NvSettingName(unsigned id) {
    switch (id) {
    case 0x10835002: return "Max Frame Rate (0 = off)";
    case 0x00A879CF: return "Vertical sync";
    case 0x007BA09E: return "Max pre-rendered frames / low latency";
    case 0x1057EB71: return "Power management mode";
    } return nullptr;
}
static void NvProfileSettings(std::string& out, NvEnum_t en, NvProfInfo_t pi, void* s, void* prof, const char* label) {
    std::vector<BYTE> info(PROFILE_SIZE, 0); *(unsigned*)info.data() = PROFILE_SIZE | (1u << 16);
    std::string pname;
    if (pi && pi(s, prof, info.data()) == 0) pname = Narrow((const wchar_t*)(info.data() + 4));
    out += Fmt("NVIDIA profile (%s): %s\r\n", label, pname.c_str());
    const unsigned CAP = 32;
    std::vector<BYTE> arr(SETTING_SIZE * CAP);
    for (unsigned start = 0; start < 2048;) {
        std::fill(arr.begin(), arr.end(), 0);
        for (unsigned i = 0; i < CAP; i++) *(unsigned*)(arr.data() + i * SETTING_SIZE) = SETTING_SIZE | (1u << 16);
        unsigned n = CAP;
        int r = en(s, prof, start, &n, arr.data());
        if (r != 0 || n == 0) { if (r != 0 && start == 0) out += Fmt("  (settings not readable: %d)\r\n", r); break; }
        for (unsigned i = 0; i < n; i++) {
            const BYTE* p = arr.data() + i * SETTING_SIZE;
            unsigned id = *(const unsigned*)(p + 0x1004), type = *(const unsigned*)(p + 0x1008), loc = *(const unsigned*)(p + 0x100C);
            const char* nm = NvSettingName(id);
            std::string val = type == 0 ? Fmt("%u (0x%X)", *(const unsigned*)(p + 0x201C), *(const unsigned*)(p + 0x201C))
                            : type == 3 ? "\"" + Narrow((const wchar_t*)(p + 0x201C)) + "\"" : Fmt("type %u", type);
            const std::string name = Narrow((const wchar_t*)(p + 4));
            out += Fmt("  0x%08X %s = %s%s\r\n", id, nm ? nm : (name.empty() ? "" : name.c_str()), val.c_str(), loc == 0 ? "" : loc == 1 ? " (from global)" : " (base/default)");
        }
        start += n;
        if (n < CAP) break;
    }
}
static void NvidiaReport(std::string& out) {
    HMODULE m = LoadLibraryW(L"nvapi.dll");
    if (!m) { out += "NVIDIA: nvapi.dll not found (no NVIDIA driver for 32-bit apps)\r\n"; return; }
    auto qi = (NvQI_t)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) { out += "NVIDIA: nvapi_QueryInterface missing\r\n"; return; }
    auto init = (NvInit_t)qi(0x0150E828);
    if (!init || init() != 0) { out += "NVIDIA: NvAPI_Initialize failed\r\n"; return; }
    if (auto drv = (NvDrv_t)qi(0x2926AAAD)) { unsigned v = 0; char br[64] = ""; if (drv(&v, br) == 0) out += Fmt("NVIDIA driver: %u.%02u (%s)\r\n", v / 100, v % 100, br); }
    auto create = (NvSess_t)qi(0x0694D52E); auto load = (NvLoad_t)qi(0x375DBD6B); auto global = (NvGlobal_t)qi(0x617BFF9F);
    auto find = (NvFindApp_t)qi(0xEEE566B2); auto en = (NvEnum_t)qi(0xAE3039DA); auto pinfo = (NvProfInfo_t)qi(0x61CD6FD6);
    auto destroy = (NvLoad_t)qi(0xDAD9CFF8);
    void* s = nullptr;
    if (!create || !load || !en || create(&s) != 0 || !s) { out += "NVIDIA: driver settings session failed\r\n"; return; }
    if (load(s) != 0) { out += "NVIDIA: loading driver settings failed\r\n"; if (destroy) destroy(s); return; }
    void* g = nullptr;
    if (global && global(s, &g) == 0 && g) NvProfileSettings(out, en, pinfo, s, g, "global");
    for (const wchar_t* exe : { L"farcry2vr.exe", L"farcry2.exe" }) {
        if (!find) break;
        void* prof = nullptr; bool found = false;
        // NVDRS_APPLICATION versions 4..1 (the driver accepts the newest it knows)
        static const unsigned sizes[] = { 20492, 16396, 16392, 12296 }; static const unsigned vers[] = { 4, 3, 2, 1 };
        for (int k = 0; k < 4 && !found; k++) {
            std::vector<BYTE> app(sizes[k], 0); *(unsigned*)app.data() = sizes[k] | (vers[k] << 16);
            wchar_t name[2048] = {}; wcsncpy(name, exe, 2047);
            int r = find(s, name, &prof, app.data());
            if (r == 0 && prof) found = true;
            else if (r != -9 /*incompatible struct version*/) break;
        }
        if (found) NvProfileSettings(out, en, pinfo, s, prof, Narrow(exe).c_str());
        else out += Fmt("NVIDIA profile (%s): none (uses the global profile)\r\n", Narrow(exe).c_str());
    }
    if (destroy) destroy(s);
}

std::string SystemReport() {
    std::string out = "\r\n---- extended system report ----\r\n";
    {   HKEY k; wchar_t cpu[128] = L"?"; DWORD sz = sizeof cpu;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) { RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr, (BYTE*)cpu, &sz); RegCloseKey(k); }
        out += "CPU: " + Narrow(cpu) + "\r\n"; }
    {   SYSTEM_POWER_STATUS ps{}; GetSystemPowerStatus(&ps);
        GUID* g = nullptr; std::string plan = "?";
        if (PowerGetActiveScheme(nullptr, &g) == ERROR_SUCCESS && g) {
            wchar_t nm[256]; DWORD sz = sizeof nm;
            if (PowerReadFriendlyName(nullptr, g, nullptr, nullptr, (UCHAR*)nm, &sz) == ERROR_SUCCESS) plan = Narrow(nm);
            LocalFree(g);
        }
        out += "Power plan: " + plan + ", power source: " + (ps.ACLineStatus == 1 ? "AC" : ps.ACLineStatus == 0 ? "BATTERY" : "unknown") + "\r\n"; }
    // GPUs with driver versions
    {   IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
                LARGE_INTEGER umd{}; std::string drv = SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))
                    ? Fmt("%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart)) : "?";
                out += Fmt("Adapter %u: %s, vendor %04X device %04X, driver %s, LUID %08lX:%08lX\r\n", i, Narrow(d.Description).c_str(), d.VendorId, d.DeviceId, drv.c_str(),
                           (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart);
                IDXGIOutput* o = nullptr;
                for (UINT j = 0; a->EnumOutputs(j, &o) == S_OK; ++j) {
                    DXGI_OUTPUT_DESC od; o->GetDesc(&od);
                    DEVMODEW dm{}; dm.dmSize = sizeof dm;
                    std::string mode = EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) ? Fmt("%lux%lu @ %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency) : "?";
                    MONITORINFOEXW mi{}; mi.cbSize = sizeof mi; GetMonitorInfoW(od.Monitor, &mi);
                    out += Fmt("  display %s: %s%s\r\n", Narrow(od.DeviceName).c_str(), mode.c_str(), (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
                    o->Release();
                }
                a->Release();
            }
            f->Release();
        } }
    NvidiaReport(out);
    // Windows graphics settings
    {   DWORD v;
        std::string hags = RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", v, KEY_WOW64_64KEY) ? (v == 2 ? "on" : v == 1 ? "off" : std::to_string(v)) : "not set";
        out += "Hardware-accelerated GPU scheduling: " + hags + "\r\n";
        out += "Game Mode (AutoGameModeEnabled): " + RegDwordText(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled") + "\r\n";
        out += "Game DVR (GameDVR_Enabled): " + RegDwordText(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled") +
               ", background capture (AppCaptureEnabled): " + RegDwordText(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled") + "\r\n";
        RegValuesMatching(out, HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", "Per-app GPU setting", { L"farcry", L"far cry", L"virtualdesktop", L"virtual desktop", L"directxuserglobalsettings" });
        RegValuesMatching(out, HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", "Compatibility flags (user)", { L"farcry", L"far cry" });
        RegValuesMatching(out, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", "Compatibility flags (all users)", { L"farcry", L"far cry" }, KEY_WOW64_64KEY);
    }
    // OpenXR runtimes and the Virtual Desktop streamer version
    {   RegValuesMatching(out, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", "OpenXR runtime installed", {}, KEY_WOW64_64KEY);
        for (const wchar_t* p : { L"C:\\Program Files\\Virtual Desktop Streamer\\VirtualDesktop.Streamer.exe" }) {
            std::string v = FileVersion(p); if (!v.empty()) out += "Virtual Desktop Streamer: " + v + "\r\n";
        } }
    // running tools that can limit, capture, overlay or compete for the GPU
    {   static const wchar_t* known[] = { L"rtss.exe", L"msiafterburner.exe", L"rtsshooksloader64.exe", L"encoderserver.exe", L"encoderserver64.exe", L"nvidia overlay.exe",
            L"nvidia app.exe", L"nvidia share.exe", L"gamebar.exe", L"gamebarftserver.exe", L"discord.exe", L"obs64.exe", L"obs32.exe", L"streamlabs obs.exe", L"medal.exe",
            L"overwolf.exe", L"losslessscaling.exe", L"wallpaper32.exe", L"wallpaper64.exe", L"amdrsserv.exe", L"radeonsoftware.exe", L"armourycrate.exe", L"lghub.exe",
            L"icue.exe", L"nzxt cam.exe", L"bes.exe", L"processlasso.exe", L"processgovernor.exe", L"presentmon.exe", L"frameview.exe", L"fpsmonitor.exe", L"razer cortex.exe",
            L"virtualdesktop.streamer.exe", L"virtualdesktop.service.exe", L"vrserver.exe", L"vrcompositor.exe", L"vrmonitor.exe", L"ovrserver_x64.exe", L"oculusclient.exe",
            L"oculusdash.exe", L"alvr_dashboard.exe", L"alvr_launcher.exe", L"steamvr.exe", L"farcry2vr.exe", L"farcry2.exe" };
        std::string found;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{ sizeof pe };
        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                std::wstring n = LowerW(pe.szExeFile);
                for (auto k : known) if (n == k) { if (found.find(Narrow(pe.szExeFile)) == std::string::npos) found += (found.empty() ? "" : ", ") + Narrow(pe.szExeFile); break; }
            }
            CloseHandle(snap);
        }
        out += "Running tools (overlays, limiters, capture, VR): " + (found.empty() ? std::string("none of the known ones") : found) + "\r\n";
    }
    return out;
}

} // namespace fc2
