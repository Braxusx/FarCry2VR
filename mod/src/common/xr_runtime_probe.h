#pragma once
// v231: which OpenXR runtime a 32-bit game gets, and whether that runtime can load into a 32-bit process.
// Far Cry 2 is a 32-bit game: its OpenXR loader reads the 32-bit registry view
// (HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime) or XR_RUNTIME_JSON. Many runtimes only register a
// 64-bit runtime there or nowhere, and the game then fails with XR_ERROR_RUNTIME_UNAVAILABLE (-51).
// Header-only, used by the mod (log) and the launcher (report, warning, runtime choice).
#include <windows.h>
#include <string>
#include <vector>

namespace xrprobe {

enum Bits { BitsUnknown = 0, Bits32 = 32, Bits64 = 64, BitsMissing = -1 };

struct Runtime {
    std::wstring json;        // manifest path
    std::wstring library;     // resolved library_path
    std::string name;         // "name" from the manifest, if any
    Bits bits = BitsUnknown;
};

inline std::wstring RegStr(HKEY root, const wchar_t* path, const wchar_t* value, REGSAM view) {
    HKEY k; if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
    wchar_t buf[2048]; DWORD sz = sizeof buf - sizeof(wchar_t), type = 0;
    std::wstring r;
    if (RegQueryValueExW(k, value, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[sz / sizeof(wchar_t)] = 0; r = buf;
        if (type == REG_EXPAND_SZ) { wchar_t ex[2048]; if (ExpandEnvironmentStringsW(buf, ex, 2048)) r = ex; }
    }
    RegCloseKey(k);
    return r;
}

inline Bits DllBits(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return BitsMissing;
    Bits r = BitsUnknown;
    IMAGE_DOS_HEADER dos; DWORD got = 0;
    if (ReadFile(h, &dos, sizeof dos, &got, nullptr) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE) {
        SetFilePointer(h, dos.e_lfanew, nullptr, FILE_BEGIN);
        DWORD sig = 0; IMAGE_FILE_HEADER fh;
        if (ReadFile(h, &sig, 4, &got, nullptr) && sig == IMAGE_NT_SIGNATURE && ReadFile(h, &fh, sizeof fh, &got, nullptr) && got == sizeof fh)
            r = fh.Machine == IMAGE_FILE_MACHINE_I386 ? Bits32 : (fh.Machine == IMAGE_FILE_MACHINE_AMD64 || fh.Machine == 0xAA64) ? Bits64 : BitsUnknown;
    }
    CloseHandle(h);
    return r;
}

// minimal: the string value of "key" in a JSON file (no full parser; manifests are tiny)
inline std::string JsonString(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k); if (p == std::string::npos) return "";
    p = j.find(':', p + k.size()); if (p == std::string::npos) return "";
    p = j.find('"', p); if (p == std::string::npos) return "";
    std::string out;
    for (size_t i = p + 1; i < j.size(); ++i) {
        char c = j[i];
        if (c == '\\' && i + 1 < j.size()) { char n = j[++i]; out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

inline std::wstring W(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n > 0 ? n - 1 : 0, 0); if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n); return w; }
inline std::string N(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n > 0 ? n - 1 : 0, 0); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr); return s; }

inline Runtime Inspect(const std::wstring& json) {
    Runtime r; r.json = json;
    if (json.empty()) { r.bits = BitsMissing; return r; }
    HANDLE h = CreateFileW(json.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { r.bits = BitsMissing; return r; }
    std::string j; char buf[4096]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got && j.size() < 65536) j.append(buf, got);
    CloseHandle(h);
    r.name = JsonString(j, "name");
    std::wstring lib = W(JsonString(j, "library_path"));
    if (lib.empty()) { r.bits = BitsMissing; return r; }
    for (auto& c : lib) if (c == L'/') c = L'\\';
    bool absolute = lib.size() > 1 && (lib[1] == L':' || (lib[0] == L'\\' && lib[1] == L'\\'));
    if (!absolute) { size_t s = json.find_last_of(L"\\/"); lib = (s == std::wstring::npos ? L"" : json.substr(0, s + 1)) + lib; }
    wchar_t full[MAX_PATH * 2]; if (GetFullPathNameW(lib.c_str(), MAX_PATH * 2, full, nullptr)) lib = full;
    r.library = lib;
    r.bits = DllBits(lib);
    return r;
}

// what a 32-bit game's OpenXR loader will use: XR_RUNTIME_JSON, else the 32-bit registry view's ActiveRuntime
inline Runtime Active32(std::wstring* source = nullptr) {
    wchar_t env[2048]; DWORD n = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 2048);
    if (n > 0 && n < 2048) { if (source) *source = L"XR_RUNTIME_JSON"; return Inspect(env); }
    if (source) *source = L"registry (32-bit view)";
    return Inspect(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_32KEY));
}
inline Runtime Active64() { return Inspect(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY)); }

// every manifest listed under AvailableRuntimes in either registry view, plus the active ones
inline std::vector<Runtime> Installed() {
    std::vector<std::wstring> seen;
    std::vector<Runtime> out;
    auto add = [&](const std::wstring& j) {
        if (j.empty()) return;
        for (auto& s : seen) if (!_wcsicmp(s.c_str(), j.c_str())) return;
        seen.push_back(j); out.push_back(Inspect(j));
    };
    for (REGSAM view : { (REGSAM)KEY_WOW64_32KEY, (REGSAM)KEY_WOW64_64KEY }) {
        add(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", view));
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
            wchar_t name[1024]; DWORD nl;
            for (DWORD i = 0;; ++i) { nl = 1024; if (RegEnumValueW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; add(name); }
            RegCloseKey(k);
        }
    }
    return out;
}

inline const char* BitsText(Bits b) { return b == Bits32 ? "32-bit" : b == Bits64 ? "64-bit only" : b == BitsMissing ? "missing" : "unknown"; }

} // namespace xrprobe
