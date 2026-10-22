// game.cpp - see game.h
#include "game.h"
#include "zip.h"
#include "payload_ids.h"
extern "C" {
#include "lzma/LzmaDec.h"
}
#include <map>
#include <mutex>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <algorithm>

namespace fc2 {

const char* kModVersion = FC2VR_VERSION;

// ---------------------------------------------------------------- strings, logging
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static bool Exists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring Join(const std::wstring& a, const std::wstring& b) { return a.empty() ? b : (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b; }

std::wstring PrefsDir() {
    wchar_t* p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = Join(p, L"FarCry2VR");
    CoTaskMemFree(p);
    if (!d.empty()) CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
void Log(const char* fmt, ...) {
    static std::wstring path = Join(PrefsDir(), L"launcher.log");
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[2200]; int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr); CloseHandle(h); }
}

// ---------------------------------------------------------------- preferences / ini
std::wstring GetPref(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024]; GetPrivateProfileStringW(section, key, def, buf, 1024, Join(PrefsDir(), L"launcher.ini").c_str()); return buf;
}
void SetPref(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), Join(PrefsDir(), L"launcher.ini").c_str());
}
std::wstring IniPath(const Game& g) { return Join(g.bin, L"fc2vr.ini"); }
std::wstring IniGet(const Game& g, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024]; GetPrivateProfileStringW(L"VR", key, def, buf, 1024, IniPath(g).c_str());
    std::wstring s = buf; size_t c = s.find(L';'); if (c != std::wstring::npos) s = s.substr(0, c);   // trailing comments
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}
void IniSet(const Game& g, const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(L"VR", key, value.c_str(), IniPath(g).c_str()); }

// ---------------------------------------------------------------- payload
// payload files are stored LZMA-compressed (.lzma "alone" format: 5 bytes properties, 8 bytes size, stream);
// unpacked on first use and kept for the launcher's lifetime
static void* LzAlloc(ISzAllocPtr, size_t n) { return malloc(n); }
static void LzFree(ISzAllocPtr, void* p) { free(p); }
Blob Payload(int id) {
    static std::mutex mx; static std::map<int, std::string> cache;
    std::lock_guard<std::mutex> lock(mx);
    auto it = cache.find(id); if (it != cache.end()) return { it->second.data(), it->second.size() };
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return {};
    HGLOBAL g = LoadResource(nullptr, r); const uint8_t* src = g ? (const uint8_t*)LockResource(g) : nullptr; size_t srcLen = g ? SizeofResource(nullptr, r) : 0;
    if (!src || srcLen < 13) return {};
    uint64_t size = 0; memcpy(&size, src + 5, 8);
    if (size > 512ull * 1024 * 1024) return {};
    std::string out((size_t)size, '\0');
    SizeT dl = (SizeT)size, sl = srcLen - 13; ELzmaStatus st; ISzAlloc al = { LzAlloc, LzFree };
    SRes res = LzmaDecode((Byte*)&out[0], &dl, src + 13, &sl, src, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &st, &al);
    if (res != SZ_OK || dl != size) { Log("payload %d: unpack failed (%d, %u of %u bytes)", id, (int)res, (unsigned)dl, (unsigned)size); return {}; }
    auto& slot = cache[id]; slot.swap(out);
    return { slot.data(), slot.size() };
}

// ---------------------------------------------------------------- files
static bool ReadFileAll(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 200ll * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart); DWORD rd = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr) && rd == sz.QuadPart);
    CloseHandle(h); return ok;
}
// writes to path.tmp, flushes, then replaces path
static DWORD WriteAtomic(const std::wstring& p, const void* data, size_t n) {
    std::wstring tmp = p + L".fc2vr-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD w = 0; BOOL ok = WriteFile(h, data, (DWORD)n, &w, nullptr) && w == n; FlushFileBuffers(h); CloseHandle(h);
    if (!ok) { DWORD e = GetLastError(); DeleteFileW(tmp.c_str()); return e ? e : ERROR_WRITE_FAULT; }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DWORD e = GetLastError(); DeleteFileW(tmp.c_str()); return e; }
    return 0;
}
static bool SameAsBlob(const std::wstring& p, const Blob& b) {
    std::string s; return b.data && ReadFileAll(p, s) && s.size() == b.size && memcmp(s.data(), b.data, b.size) == 0;
}
static bool CanWrite(const std::wstring& dir, bool& denied) {
    std::wstring p = Join(dir, L".fc2vr-write-probe.tmp");
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) { denied = GetLastError() == ERROR_ACCESS_DENIED; return false; }
    CloseHandle(h); denied = false; return true;
}
static bool IsElevated() {
    HANDLE t; TOKEN_ELEVATION e{}; DWORD n = 0; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { if (GetTokenInformation(t, TokenElevation, &e, sizeof e, &n)) r = e.TokenIsElevated != 0; CloseHandle(t); }
    return r;
}

// ---------------------------------------------------------------- discovery
static std::wstring RegString(HKEY root, const wchar_t* path, const wchar_t* value, REGSAM view) {
    HKEY k; if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[1024]; DWORD n = sizeof buf, type = 0; std::wstring r;
    if (RegQueryValueExW(k, value, nullptr, &type, (BYTE*)buf, &n) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) { buf[std::min<DWORD>(n / 2, 1023)] = 0; r = buf; }
    RegCloseKey(k); return r;
}
static Build DuniaBuild(const std::wstring& bin) {
    HANDLE h = CreateFileW(Join(bin, L"Dunia.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return BuildUnknown;
    uint8_t hdr[0x400]; DWORD rd = 0; ReadFile(h, hdr, sizeof hdr, &rd, nullptr); CloseHandle(h);
    if (rd < 0x100) return BuildUnknown;
    uint32_t pe = *(uint32_t*)(hdr + 0x3C); if (pe + 0x60 > rd) return BuildUnknown;
    uint32_t ts = *(uint32_t*)(hdr + pe + 8), size = *(uint32_t*)(hdr + pe + 24 + 56);
    if (ts == 0x4AAE9636 || size == 0x019E8000) return BuildSteam;
    if (ts == 0x49FB4BF6 || size == 0x0192A000) return BuildGog;
    return BuildUnknown;
}
Game Inspect(const std::wstring& dirIn) {
    Game g; std::wstring d = dirIn;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (Lower(d).size() > 4 && Lower(d).substr(Lower(d).size() - 4) == L"\\bin") { g.bin = d; g.root = d.substr(0, d.size() - 4); }
    else { g.root = d; g.bin = Join(d, L"bin"); }
    g.valid = Exists(Join(g.bin, L"FarCry2.exe")) && Exists(Join(g.bin, L"Dunia.dll"));
    if (!g.valid) return g;
    g.build = DuniaBuild(g.bin);
    std::wstring l = Lower(g.root);
    if (l.find(L"steamapps\\common") != std::wstring::npos) g.store = StoreSteam;
    else {
        WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(Join(g.bin, L"goggame-*.dll").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) f = FindFirstFileW(Join(g.root, L"goggame-*.info").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) { g.store = StoreGog; FindClose(f); }
        else if (g.build == BuildSteam) g.store = StoreSteam;
        else if (g.build == BuildGog) g.store = StoreGog;
    }
    return g;
}
std::vector<Game> FindGames() {
    std::vector<std::wstring> dirs;
    // Steam: every library folder
    std::vector<std::wstring> steam;
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
        std::wstring a = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", v); if (!a.empty()) steam.push_back(a);
        std::wstring b = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", v); if (!b.empty()) steam.push_back(b);
    }
    std::vector<std::wstring> libs;
    for (auto& s : steam) {
        std::wstring p = s; for (auto& c : p) if (c == L'/') c = L'\\';
        libs.push_back(p);
        std::string vdf; if (!ReadFileAll(Join(p, L"steamapps\\libraryfolders.vdf"), vdf)) continue;
        size_t pos = 0;
        while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
            size_t a = vdf.find('"', pos + 6); if (a == std::string::npos) break;
            size_t b = vdf.find('"', a + 1); if (b == std::string::npos) break;
            std::string v = vdf.substr(a + 1, b - a - 1), u;
            for (size_t i = 0; i < v.size(); ++i) { if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i; u += v[i]; }
            libs.push_back(Widen(u)); pos = b + 1;
        }
    }
    for (auto& l : libs) {
        std::wstring dir = Join(l, L"steamapps\\common\\Far Cry 2");
        std::string acf;
        if (ReadFileAll(Join(l, L"steamapps\\appmanifest_19900.acf"), acf)) {
            size_t p = acf.find("\"installdir\"");
            if (p != std::string::npos) { size_t a = acf.find('"', p + 12), b = a == std::string::npos ? a : acf.find('"', a + 1); if (b != std::string::npos) dir = Join(l, L"steamapps\\common\\" + Widen(acf.substr(a + 1, b - a - 1))); }
        }
        dirs.push_back(dir);
    }
    // GOG: Galaxy / installer registry entries
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY })
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
            HKEY k; if (RegOpenKeyExW(root, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | v, &k) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0; ; ++i) {
                DWORD n = 256; if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::wstring sub = std::wstring(L"SOFTWARE\\GOG.com\\Games\\") + name;
                std::wstring gn = Lower(RegString(root, sub.c_str(), L"gameName", v));
                if (gn.find(L"far cry 2") != std::wstring::npos || wcscmp(name, L"1207659042") == 0) { std::wstring p = RegString(root, sub.c_str(), L"path", v); if (!p.empty()) dirs.push_back(p); }
            }
            RegCloseKey(k);
        }
    // uninstall entries (retail, Ubisoft, other installers)
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
        HKEY k; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | v, &k) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0; ; ++i) {
            DWORD n = 256; if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring sub = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
            std::wstring dn = Lower(RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"DisplayName", v));
            if (dn.find(L"far cry 2") != std::wstring::npos) { std::wstring p = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallLocation", v); if (!p.empty()) dirs.push_back(p); }
        }
        RegCloseKey(k);
    }
    std::wstring last = GetPref(L"Game", L"Directory", L""); if (!last.empty()) dirs.insert(dirs.begin(), last);
    std::vector<Game> out;
    for (auto& d : dirs) {
        Game g = Inspect(d); if (!g.valid) continue;
        bool dup = false; for (auto& o : out) dup |= Lower(o.bin) == Lower(g.bin);
        if (!dup) out.push_back(g);
    }
    return out;
}

// ---------------------------------------------------------------- state
static const wchar_t* kRecord = L"fc2vr_install.txt";
static const wchar_t* kBackup = L"dinput8.dll.fc2vr-backup";
State Check(const Game& g) {
    State s;
    if (!g.valid) return s;
    std::string rec; bool hasRec = ReadFileAll(Join(g.bin, kRecord), rec);
    if (hasRec) { size_t p = rec.find("version="); if (p != std::string::npos) { size_t e = rec.find_first_of("\r\n", p); s.installedVersion = rec.substr(p + 8, e == std::string::npos ? std::string::npos : e - p - 8); } }
    bool dll = Exists(Join(g.bin, L"dinput8.dll"));
    bool ours = dll && (hasRec || SameAsBlob(Join(g.bin, L"dinput8.dll"), Payload(IDR_DINPUT8)) || Exists(Join(g.bin, L"fc2vr.log")));
    s.installed = ours;
    s.outdated = ours && hasRec && !SameAsBlob(Join(g.bin, L"dinput8.dll"), Payload(IDR_DINPUT8));
    s.foreignDinput = dll && !ours;
    s.backupPresent = Exists(Join(g.bin, kBackup));
    s.iniExists = Exists(IniPath(g));
    s.vrExe = Exists(Join(g.bin, L"FarCry2VR.exe"));
    bool denied = false; if (!CanWrite(g.bin, denied) && denied && !IsElevated()) s.needsElevation = true;
    return s;
}
bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof pe }; bool r = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"FarCry2.exe") || !_wcsicmp(pe.szExeFile, L"FarCry2VR.exe")) { r = true; break; }
    CloseHandle(snap); return r;
}

// ---------------------------------------------------------------- install / uninstall
struct PayloadFile { int id; const wchar_t* rel; };
static const PayloadFile kFiles[] = {
    { IDR_DINPUT8, L"dinput8.dll" }, { IDR_OPENXR, L"openxr_loader.dll" },
    { IDR_DLSS_HOST, L"fc2vr_dlss\\fc2vr_dlss_host64.exe" }, { IDR_NGX_DLSS, L"fc2vr_dlss\\nvngx_dlss.dll" },
};
static void Add(Report& r, char kind, const std::string& text) { r.lines.push_back(std::string(1, kind) + " " + text); Log("%c %s", kind, text.c_str()); if (kind == 'x') r.ok = false; }
static std::string ErrText(DWORD e) {
    wchar_t* m = nullptr; FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&m, 0, nullptr);
    std::string s = m ? Narrow(m) : "error " + std::to_string(e); LocalFree(m);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}
// FarCry2VR.exe: FarCry2.exe with the large-address-aware flag (4 GB of address space for the 32-bit game)
static DWORD MakeVrExe(const Game& g) {
    std::string exe; if (!ReadFileAll(Join(g.bin, L"FarCry2.exe"), exe) || exe.size() < 0x200) return ERROR_FILE_NOT_FOUND;
    uint32_t pe = *(uint32_t*)&exe[0x3C]; if (pe + 0x60 > exe.size() || memcmp(&exe[pe], "PE\0\0", 4)) return ERROR_BAD_FORMAT;
    *(uint16_t*)&exe[pe + 22] |= 0x0020;
    uint32_t co = pe + 24 + 64; *(uint32_t*)&exe[co] = 0;
    uint64_t sum = 0; size_t n = exe.size() & ~(size_t)1;
    for (size_t i = 0; i < n; i += 2) { sum += *(uint16_t*)&exe[i]; sum = (sum & 0xFFFF) + (sum >> 16); }
    if (exe.size() & 1) { sum += (uint8_t)exe.back(); sum = (sum & 0xFFFF) + (sum >> 16); }
    sum = (sum & 0xFFFF) + (sum >> 16); sum += exe.size();
    *(uint32_t*)&exe[co] = (uint32_t)sum;
    return WriteAtomic(Join(g.bin, L"FarCry2VR.exe"), exe.data(), exe.size());
}
// adds keys the user's ini doesn't have yet (new options) without touching their values
static int MergeIniDefaults(const Game& g, const Blob& b) {
    std::string s((const char*)b.data, b.size); std::wstring section; int added = 0; size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
        std::string line = s.substr(p, e - p); p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { size_t c = line.find(']'); section = Widen(line.substr(1, c == std::string::npos ? std::string::npos : c - 1)); continue; }
        size_t eq = line.find('='); if (eq == std::string::npos || section.empty()) continue;
        std::wstring key = Widen(line.substr(0, eq)), val = Widen(line.substr(eq + 1));
        wchar_t buf[8]; GetPrivateProfileStringW(section.c_str(), key.c_str(), L"\x01", buf, 8, IniPath(g).c_str());
        if (buf[0] == 1) { WritePrivateProfileStringW(section.c_str(), key.c_str(), val.c_str(), IniPath(g).c_str()); ++added; }
    }
    return added;
}
Report Install(const Game& g) {
    Report r;
    Log("install %s (%s build) version %s", Narrow(g.root).c_str(), g.build == BuildSteam ? "Steam" : g.build == BuildGog ? "GOG" : "unknown", kModVersion);
    if (!g.valid) { Add(r, 'x', "Far Cry 2 wasn't found in that folder (bin\\FarCry2.exe and bin\\Dunia.dll)."); return r; }
    if (GameRunning()) { Add(r, 'x', "Far Cry 2 is running. Close the game first."); return r; }
    if (g.build == BuildUnknown) Add(r, '!', "This Far Cry 2 version isn't one the mod was made for (Steam or GOG 1.03). It may not work.");
    for (const auto& f : kFiles) if (!Payload(f.id).data) { Add(r, 'x', "The launcher is incomplete (missing " + Narrow(f.rel) + ")."); return r; }
    State s = Check(g);
    if (s.foreignDinput) {
        DWORD e = MoveFileExW(Join(g.bin, L"dinput8.dll").c_str(), Join(g.bin, kBackup).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : GetLastError();
        if (e) { Add(r, 'x', "Couldn't move the existing dinput8.dll aside: " + ErrText(e)); r.accessDenied = e == ERROR_ACCESS_DENIED; return r; }
        Add(r, '+', "Another mod's dinput8.dll was kept as dinput8.dll.fc2vr-backup (restored on uninstall)");
    }
    CreateDirectoryW(Join(g.bin, L"fc2vr_dlss").c_str(), nullptr);
    for (const auto& f : kFiles) {
        Blob b = Payload(f.id);
        DWORD e = WriteAtomic(Join(g.bin, f.rel), b.data, b.size);
        if (e) { Add(r, 'x', "Couldn't write " + Narrow(f.rel) + ": " + ErrText(e)); r.accessDenied |= e == ERROR_ACCESS_DENIED; return r; }
    }
    Add(r, '+', "Mod files installed (dinput8.dll, openxr_loader.dll, fc2vr_dlss)");
    Blob ini = Payload(IDR_INI);
    if (!Exists(IniPath(g))) {
        DWORD e = WriteAtomic(IniPath(g), ini.data, ini.size);
        if (e) { Add(r, 'x', "Couldn't write fc2vr.ini: " + ErrText(e)); return r; }
        Add(r, '+', "Settings file created (fc2vr.ini)");
    } else {
        int n = MergeIniDefaults(g, ini);
        Add(r, '+', n ? "Your settings were kept; " + std::to_string(n) + " new option(s) added to fc2vr.ini" : std::string("Your settings in fc2vr.ini were kept"));
    }
    DWORD e = MakeVrExe(g);
    if (e) { Add(r, 'x', "Couldn't create FarCry2VR.exe: " + ErrText(e)); return r; }
    Add(r, '+', "FarCry2VR.exe created (the game with 4 GB memory access)");
    std::string rec = std::string("version=") + kModVersion + "\r\nbuild=" + (g.build == BuildSteam ? "steam" : g.build == BuildGog ? "gog" : "unknown") + "\r\n";
    for (const auto& f : kFiles) rec += "file=" + Narrow(f.rel) + "\r\n";
    rec += "file=FarCry2VR.exe\r\n";
    WriteAtomic(Join(g.bin, kRecord), rec.data(), rec.size());
    Add(r, '+', std::string("Far Cry 2 VR ") + kModVersion + " is installed");
    return r;
}
Report Uninstall(const Game& g, bool deleteIni) {
    Report r;
    Log("uninstall %s (delete ini %d)", Narrow(g.root).c_str(), deleteIni);
    if (GameRunning()) { Add(r, 'x', "Far Cry 2 is running. Close the game first."); return r; }
    State s = Check(g);
    auto del = [&](const std::wstring& rel) -> bool {
        std::wstring p = Join(g.bin, rel);
        if (!Exists(p)) return true;
        if (DeleteFileW(p.c_str())) return true;
        DWORD e = GetLastError(); Add(r, 'x', "Couldn't remove " + Narrow(rel) + ": " + ErrText(e)); r.accessDenied |= e == ERROR_ACCESS_DENIED; return false;
    };
    if (s.installed || Exists(Join(g.bin, kRecord))) { if (!del(L"dinput8.dll")) return r; }
    else if (Exists(Join(g.bin, L"dinput8.dll"))) Add(r, '!', "dinput8.dll belongs to another mod and was left in place");
    for (const auto& f : kFiles) if (f.id != IDR_DINPUT8 && !del(f.rel)) return r;
    for (const wchar_t* f : { L"fc2vr_dlss\\dlss_host.log", L"fc2vr_dlss\\dlss_host.prev.log", L"FarCry2VR.exe", L"fc2vr.log", L"fc2vr_prev.log" }) if (!del(f)) return r;
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(Join(g.bin, L"fc2vr_hang_*.txt").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do del(fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); } }
    RemoveDirectoryW(Join(g.bin, L"fc2vr_dlss").c_str());   // only if empty
    Add(r, '+', "Mod files removed");
    if (s.backupPresent) {
        if (MoveFileExW(Join(g.bin, kBackup).c_str(), Join(g.bin, L"dinput8.dll").c_str(), MOVEFILE_REPLACE_EXISTING)) Add(r, '+', "The other mod's dinput8.dll was put back");
        else Add(r, '!', "Couldn't restore dinput8.dll.fc2vr-backup: " + ErrText(GetLastError()));
    }
    if (deleteIni) { if (del(L"fc2vr.ini")) Add(r, '+', "Settings file removed"); }
    else if (Exists(IniPath(g))) Add(r, '+', "fc2vr.ini was kept (your settings for a later reinstall)");
    del(kRecord);
    Add(r, '+', "Far Cry 2 VR is uninstalled. Your save games weren't touched.");
    return r;
}

// result file of an elevated run: "ok=1 denied=0", then one report line per line
static void WriteResult(const std::wstring& path, const Report& r) {
    std::string s = "ok=" + std::to_string(r.ok) + " denied=" + std::to_string(r.accessDenied) + "\n";
    for (auto& l : r.lines) s += l + "\n";
    WriteAtomic(path, s.data(), s.size());
}
static bool ReadResult(const std::wstring& path, Report& r) {
    std::string s; if (!ReadFileAll(path, s) || s.size() < 4) return false;
    r = Report{}; r.ok = s.find("ok=1") == 0; r.accessDenied = s.find("denied=1") != std::string::npos;
    size_t p = s.find('\n'); while (p != std::string::npos && p + 1 < s.size()) { size_t e = s.find('\n', p + 1); r.lines.push_back(s.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1)); p = e; }
    return true;
}
int ElevatedMain(int argc, wchar_t** argv);   // used by main.cpp
int ElevatedMain(int argc, wchar_t** argv) {
    std::wstring op, game, result; bool delIni = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--op") && i + 1 < argc) op = argv[++i];
        else if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game = argv[++i];
        else if (!wcscmp(argv[i], L"--result") && i + 1 < argc) result = argv[++i];
        else if (!wcscmp(argv[i], L"--delete-ini")) delIni = true;
    }
    Game g = Inspect(game);
    Report r = op == L"install" ? Install(g) : op == L"uninstall" ? Uninstall(g, delIni) : Report{ false, false, { "x unknown operation" } };
    if (!result.empty()) WriteResult(result, r);
    return r.ok ? 0 : r.accessDenied ? 3 : 2;
}
Report RunOperation(const Game& g, const char* op, bool deleteIni) {
    State s = Check(g);
    if (!s.needsElevation) {
        Report r = !strcmp(op, "install") ? Install(g) : Uninstall(g, deleteIni);
        if (!(r.accessDenied && !IsElevated())) return r;
    }
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring result = Join(tmp, L"fc2vr-launcher-result-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    DeleteFileW(result.c_str());
    std::wstring params = L"--op " + Widen(op) + L" --game \"" + g.root + L"\" --result \"" + result + L"\"" + (deleteIni ? L" --delete-ini" : L"");
    SHELLEXECUTEINFOW sei{ sizeof sei }; sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = params.c_str(); sei.nShow = SW_HIDE;
    Log("asking for administrator rights: %s", Narrow(params).c_str());
    if (!ShellExecuteExW(&sei)) {
        Report r; r.ok = false;
        r.lines.push_back(GetLastError() == ERROR_CANCELLED ? "x Administrator rights were declined, so nothing was changed." : "x Couldn't start the installer with administrator rights: " + ErrText(GetLastError()));
        return r;
    }
    WaitForSingleObject(sei.hProcess, 10 * 60 * 1000); CloseHandle(sei.hProcess);
    Report r; if (!ReadResult(result, r)) { r.ok = false; r.lines.push_back("x The elevated installer didn't report back."); }
    DeleteFileW(result.c_str());
    return r;
}

// ---------------------------------------------------------------- launch
bool Launch(const Game& g, const LaunchOptions& o, std::string& err) {
    std::wstring exe = Join(g.bin, L"FarCry2VR.exe");
    if (!Exists(exe)) exe = Join(g.bin, L"FarCry2.exe");
    std::wstring cmd = L"\"" + exe + L"\"";
    if (o.skipIntro) cmd += L" -GameProfile_SkipIntroMovies 1";
    if (o.godMode) cmd += L" -GameProfile_GodMode 1";
    if (o.unlimitedAmmo) cmd += L" -GameProfile_UnlimitedAmmo 1";
    if (o.noJam) cmd += L" -GameProfile_UnlimitedReliability 1";
    if (o.allWeapons) cmd += L" -GameProfile_AllWeaponsUnlock 1";
    STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    Log("launch: %s (core limit %d)", Narrow(cmd).c_str(), o.coreLimit);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, g.bin.c_str(), &si, &pi)) { err = "Couldn't start the game: " + ErrText(GetLastError()); Log("%s", err.c_str()); return false; }
    // the game doesn't start on more than 16 logical processors: keep it to the first coreLimit of them
    DWORD_PTR procMask = 0, sysMask = 0;
    if (o.coreLimit > 0 && GetProcessAffinityMask(pi.hProcess, &procMask, &sysMask)) {
        DWORD_PTR m = 0; int n = 0;
        for (int b = 0; b < (int)(sizeof(DWORD_PTR) * 8) && n < o.coreLimit; ++b) if (procMask & ((DWORD_PTR)1 << b)) { m |= (DWORD_PTR)1 << b; ++n; }
        if (m && m != procMask) SetProcessAffinityMask(pi.hProcess, m);
    }
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------- log collection
std::wstring CollectLogs(const Game& g, std::string& err) {
    wchar_t* desk = nullptr; std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dir = desk;
    CoTaskMemFree(desk);
    if (dir.empty()) { err = "The desktop folder wasn't found."; return {}; }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[96]; swprintf(name, 96, L"FC2VR-Logs-%04d%02d%02d-%02d%02d%02d.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring zipPath = Join(dir, name);
    ZipWriter z; if (!z.Open(zipPath)) { err = "Couldn't create " + Narrow(zipPath); return {}; }
    // system summary
    std::string info = "Far Cry 2 VR launcher " + std::string(kModVersion) + "\r\n";
    { typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW*); OSVERSIONINFOW v{ sizeof v };
      auto f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
      if (f && !f(&v)) info += "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " + std::to_string(v.dwBuildNumber) + "\r\n"; }
    { SYSTEM_INFO si; GetNativeSystemInfo(&si); info += "Logical processors: " + std::to_string(si.dwNumberOfProcessors) + "\r\n";
      MEMORYSTATUSEX ms{ sizeof ms }; GlobalMemoryStatusEx(&ms); info += "RAM: " + std::to_string((unsigned)(ms.ullTotalPhys >> 20)) + " MB\r\n"; }
    { IDXGIFactory1* f = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
          IDXGIAdapter1* a = nullptr;
          for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) { DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); info += "GPU " + std::to_string(i) + ": " + Narrow(d.Description) + " (" + std::to_string((unsigned)(d.DedicatedVideoMemory >> 20)) + " MB)\r\n"; a->Release(); }
          f->Release(); } }
    info += "OpenXR runtime: " + Narrow(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY)) + "\r\n";
    info += "Game: " + Narrow(g.root) + " (" + (g.store == StoreSteam ? "Steam" : g.store == StoreGog ? "GOG" : "other") + ", Dunia " + (g.build == BuildSteam ? "Steam" : g.build == BuildGog ? "GOG" : "unknown") + ")\r\n";
    State s = Check(g);
    info += "Mod installed: " + std::string(s.installed ? "yes " + s.installedVersion : "no") + ", FarCry2VR.exe: " + (s.vrExe ? "yes" : "no") + ", other dinput8: " + (s.foreignDinput ? "yes" : "no") + "\r\n";
    z.Add("system-info.txt", info.data(), info.size());
    int n = 0;
    auto addFile = [&](const std::wstring& path, const std::string& name) { std::string d; if (ReadFileAll(path, d)) { z.Add(name, d.data(), d.size()); ++n; } };
    if (g.valid) {
        for (const wchar_t* f : { L"fc2vr.log", L"fc2vr_prev.log", L"fc2vr.ini", L"fc2vr_install.txt" }) addFile(Join(g.bin, f), Narrow(f));
        addFile(Join(g.bin, L"fc2vr_dlss\\dlss_host.log"), "dlss_host.log"); addFile(Join(g.bin, L"fc2vr_dlss\\dlss_host.prev.log"), "dlss_host.prev.log");
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(Join(g.bin, L"fc2vr_hang_*.txt").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do addFile(Join(g.bin, fd.cFileName), Narrow(fd.cFileName)); while (FindNextFileW(h, &fd)); FindClose(h); }
    }
    addFile(Join(PrefsDir(), L"launcher.log"), "launcher.log");
    wchar_t* docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) addFile(Join(docs, L"My Games\\Far Cry 2\\GamerProfile.xml"), "GamerProfile.xml");
    CoTaskMemFree(docs);
    if (!z.Close()) { err = "Writing the zip failed."; return {}; }
    Log("logs collected: %s (%d files)", Narrow(zipPath).c_str(), n);
    return zipPath;
}

} // namespace fc2
