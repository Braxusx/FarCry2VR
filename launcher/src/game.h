// game.h - Far Cry 2 discovery, mod install / uninstall, launch and launcher preferences.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace fc2 {

enum Store { StoreUnknown = 0, StoreSteam = 1, StoreGog = 2 };
enum Build { BuildUnknown = 0, BuildSteam = 1, BuildGog = 2 };

struct Game {
    std::wstring root;        // game folder (contains bin)
    std::wstring bin;         // root\bin
    Store store = StoreUnknown;
    Build build = BuildUnknown;   // from Dunia.dll's PE time stamp
    bool valid = false;       // bin\FarCry2.exe and bin\Dunia.dll present
};

struct State {
    bool installed = false;       // our dinput8.dll / install record present
    bool foreignDinput = false;   // a dinput8.dll that isn't ours (another mod)
    bool backupPresent = false;   // the original dinput8.dll we moved aside
    bool iniExists = false;
    bool vrExe = false;           // FarCry2VR.exe present
    bool needsElevation = false;  // the bin folder can't be written without admin rights
    bool outdated = false;        // installed mod files differ from the ones this launcher carries
    std::string installedVersion;
};

struct Report {
    bool ok = true, accessDenied = false;
    std::vector<std::string> lines;   // "+ text" done, "! text" warning, "x text" failed
};

// payload embedded in the launcher (RCDATA)
struct Blob { const void* data = nullptr; size_t size = 0; };
Blob Payload(int id);

std::vector<Game> FindGames();
Game Inspect(const std::wstring& dir);   // accepts the game folder or its bin folder
State Check(const Game& g);
bool GameRunning();

Report Install(const Game& g);
Report Uninstall(const Game& g, bool deleteIni);
// runs the operation in an elevated copy of the launcher when the folder needs admin rights
Report RunOperation(const Game& g, const char* op, bool deleteIni);

struct LaunchOptions { bool godMode = false, unlimitedAmmo = false, noJam = false, allWeapons = false, skipIntro = true; int coreLimit = 16; std::wstring xrRuntime; };   // xrRuntime: OpenXR manifest for the game (XR_RUNTIME_JSON), empty = Windows default
bool Launch(const Game& g, const LaunchOptions& o, std::string& err);

// launcher preferences (%LOCALAPPDATA%\FarCry2VR\launcher.ini)
std::wstring PrefsDir();
std::wstring GetPref(const wchar_t* section, const wchar_t* key, const wchar_t* def);
void SetPref(const wchar_t* section, const wchar_t* key, const std::wstring& value);

// the mod's ini in the game's bin folder
std::wstring IniPath(const Game& g);
std::wstring IniGet(const Game& g, const wchar_t* key, const wchar_t* def);
void IniSet(const Game& g, const wchar_t* key, const std::wstring& value);

std::string Narrow(const std::wstring& w);
std::string SystemReport();   // sysreport.cpp: extended system report for the log zip
std::wstring Widen(const std::string& s);
void Log(const char* fmt, ...);
extern const char* kModVersion;

// Help: collects the logs into a zip on the desktop; returns its path (empty on failure)
std::wstring CollectLogs(const Game& g, std::string& err);

} // namespace fc2
